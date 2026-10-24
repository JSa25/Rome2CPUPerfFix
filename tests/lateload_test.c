// Test for the late install (proxy.c, wait_for_game): the built dinput8.dll is loaded first, as when an overlay or
// another hook DLL pulls dinput8.dll in early, and the game DLL afterwards. Its preferred address is taken first, so
// Windows has to relocate it: the fixes must install from the load notification, with every byte check passing (they
// compare relocated code, so a check made before relocation would fail) and the patches in place afterwards.
// The game DLL's own startup code runs too (its imports resolve from the game folder).
// usage: lateload_test.exe <path to the built dinput8.dll> <path to empire.retail.dll>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static char g_log[1 << 16];
static int has(const char *s) { return strstr(g_log, s) != NULL; }

// Where the jmp at a patched site goes.
static unsigned char *target(unsigned char *site) {
    int rel;
    memcpy(&rel, site + 1, 4);
    return site + 5 + rel;
}

int main(int argc, char **argv) {
    CHECK(argc == 3);
    char logp[MAX_PATH];
    strcpy(logp, argv[1]);
    char *sl = strrchr(logp, '\\');
    CHECK(sl);
    strcpy(sl + 1, "rome2fix.log");
    DeleteFileA(logp);

    void *taken = VirtualAlloc((void *)0x10000000, 0x100000, MEM_RESERVE, PAGE_NOACCESS);
    CHECK(taken == (void *)0x10000000);
    HMODULE me = LoadLibraryA(argv[1]);
    CHECK(me);
    CHECK(GetModuleHandleA("empire.retail.dll") == NULL);
    // a normal load (the notification is not sent for DONT_RESOLVE_DLL_REFERENCES), with the game folder on the
    // search path for its other imports; its DINPUT8.dll import binds to the copy already loaded
    char gdir[MAX_PATH];
    strcpy(gdir, argv[2]);
    char *gs = strrchr(gdir, '\\');
    if (!gs) gs = strrchr(gdir, '/');
    CHECK(gs);
    *gs = 0;
    SetDllDirectoryA(gdir);
    HMODULE game = LoadLibraryA(argv[2]);
    CHECK(game && game != (HMODULE)0x10000000);

    FILE *f = fopen(logp, "rb");
    CHECK(f);
    g_log[fread(g_log, 1, sizeof g_log - 1, f)] = 0;
    fclose(f);
    fputs(g_log, stdout);
    CHECK(has("was loaded before empire.retail.dll"));
    CHECK(has("empire.retail.dll is loading now - installing the fixes"));
    CHECK(has("Rome 2 CPU Performance Fix v"));
    CHECK(has("tile feedback scan replaced"));
    CHECK(has("x87 fix: 1196 of 1196"));
    CHECK(has("draw-list sort replaced") && has("render-list sort (+0x44/+0x40) replaced"));
    CHECK(!has("does not match") && !has("skipped") && !has("not installed"));

    unsigned char *b = (unsigned char *)game, *lo = (unsigned char *)me;
    unsigned char *hi = lo + ((IMAGE_NT_HEADERS *)(lo + ((IMAGE_DOS_HEADER *)lo)->e_lfanew))->OptionalHeader.SizeOfImage;
    const DWORD into_dll[] = {0x1594020, 0x1594065, 0x1594198, 0x14A8370, 0x14A7930};  // tile scan (3), sorts (2)
    for (int i = 0; i < 5; i++) {
        unsigned char *site = b + into_dll[i];
        CHECK(site[0] == 0xE9 && target(site) >= lo && target(site) < hi);
    }
    printf("late install: game DLL relocated to %p, all fixes installed from the load notification\n", (void *)game);
    printf("all late-load tests passed\n");
    return 0;
}
