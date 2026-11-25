// One engine fix per game (proxy.c DllMain, Local\Rome2EngineFix_<pid>; from Attila 7.4): the
// DLL loaded twice into one process, as two files in two folders (a second dinput8.dll elsewhere, one in plugins\, one
// under another name). The first copy must start as usual (here: no empire.retail.dll, so it waits for the game), the
// second must log that it stays out and nothing else; both must still forward DirectInput8Create to Windows' dinput8.
// usage: dupload_test.exe <dinput8.dll>
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define CHECK(c, ...) do { if (!(c)) { printf("FAILED: " __VA_ARGS__); putchar('\n'); return 1; } } while (0)

static char g_text[65536];
static const char *read_log(const char *dir) {
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%s\\rome2fix.log", dir);
    FILE *f = fopen(p, "rb");
    g_text[0] = 0;
    if (f) { g_text[fread(g_text, 1, sizeof g_text - 1, f)] = 0; fclose(f); }
    return g_text;
}

int main(int argc, char **argv) {
    CHECK(argc == 2, "usage: dupload_test.exe <dinput8.dll>");
    char here[MAX_PATH], a[MAX_PATH], b[MAX_PATH], fa[MAX_PATH], fb[MAX_PATH];
    GetModuleFileNameA(NULL, here, MAX_PATH);
    *strrchr(here, '\\') = 0;
    snprintf(a, sizeof a, "%s\\dup_a", here);
    snprintf(b, sizeof b, "%s\\dup_b", here);
    CreateDirectoryA(a, NULL);
    CreateDirectoryA(b, NULL);
    snprintf(fa, sizeof fa, "%s\\dinput8.dll", a);
    snprintf(fb, sizeof fb, "%s\\enginefix_copy.dll", b);   // the second copy under another name
    CHECK(CopyFileA(argv[1], fa, FALSE) && CopyFileA(argv[1], fb, FALSE), "copy the DLL");
    for (int i = 0; i < 2; i++) {
        char p[MAX_PATH];
        snprintf(p, sizeof p, "%s\\rome2fix.log", i ? b : a);
        DeleteFileA(p);
        snprintf(p, sizeof p, "%s\\rome2fix.ini", i ? b : a);
        DeleteFileA(p);
    }
    HMODULE ma = LoadLibraryA(fa), mb = LoadLibraryA(fb);
    CHECK(ma && mb && ma != mb, "both copies load");
    const char *la = read_log(a);
    int a_ok = strstr(la, "loaded before empire.retail.dll") != NULL && !strstr(la, "already running");
    printf("first copy: %s\n", a_ok ? "started (waits for the game)" : "wrong");
    CHECK(a_ok, "the first copy did not start as usual:\n%s", la);
    const char *lb = read_log(b);
    int b_ok = strstr(lb, "another copy is already running in this game (Local\\Rome2EngineFix_") && strstr(lb, "stays out") &&
               !strstr(lb, "loaded before empire.retail.dll") && !strstr(lb, "game started");
    printf("second copy: %s\n", b_ok ? "stays out" : "wrong");
    CHECK(b_ok, "the second copy did not stay out:\n%s", lb);
    typedef HRESULT(WINAPI * create_fn)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
    create_fn ca = (create_fn)(void *)GetProcAddress(ma, "DirectInput8Create"), cb = (create_fn)(void *)GetProcAddress(mb, "DirectInput8Create");
    CHECK(ca && cb, "DirectInput8Create exported by both");
    static const GUID iid_di8w = {0xBF798031, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
    void *da = NULL, *db = NULL;
    CHECK(SUCCEEDED(ca(GetModuleHandleW(NULL), 0x0800, &iid_di8w, &da, NULL)) && da, "the first copy's DirectInput8Create");
    CHECK(SUCCEEDED(cb(GetModuleHandleW(NULL), 0x0800, &iid_di8w, &db, NULL)) && db, "the second copy's DirectInput8Create");
    printf("dupload ok: one engine fix per process, the second copy forwards only\n");
    return 0;
}
