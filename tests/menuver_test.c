// Tests menuver.c on a real empire.retail.dll (mapped, not run; each known build): the main menu's `mov ecx,[s_ui]` is
// found, the pointer lies in the image's .data, the label follows it (0 while null, 1 once set), and the same image
// with one byte of that code changed is refused. A broken copy (no byte check after the load) must fail it.
// usage: menuver_test.exe <path to empire.retail.dll>
#include "../menuver.c"
#include <stdio.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)
int (*g_fgp_menu_label)(void);   // fgpanel.c's
static void nolog(const char *fmt, ...) { (void)fmt; }

int main(int argc, char **argv) {
    CHECK(argc == 2);
    wchar_t w[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, argv[1], -1, w, MAX_PATH);
    unsigned char *b = (unsigned char *)LoadLibraryExW(w, NULL, DONT_RESOLVE_DLL_REFERENCES);
    CHECK(b != NULL);
    CHECK(install_menuver((HMODULE)b, nolog) == 1);
    DWORD rva = (DWORD)((const unsigned char *)g_s_ui - b);
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(b + ((const IMAGE_DOS_HEADER *)b)->e_lfanew);
    const IMAGE_SECTION_HEADER *sh = IMAGE_FIRST_SECTION(nt), *in = NULL;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (rva >= sh[i].VirtualAddress && rva + 4 <= sh[i].VirtualAddress + sh[i].Misc.VirtualSize) in = &sh[i];
    CHECK(in && !memcmp(in->Name, ".data", 6));
    CHECK(g_fgp_menu_label == menus_up && !g_fgp_menu_label());   // the game never ran: no front end
    DWORD prot;
    CHECK(VirtualProtect((void *)g_s_ui, 4, PAGE_READWRITE, &prot));
    *(void **)g_s_ui = (void *)b;   // what the constructor stores
    CHECK(g_fgp_menu_label());
    *(void **)g_s_ui = NULL;         // the destructor
    CHECK(!g_fgp_menu_label());
    int build = -1;
    for (int i = 0; i < NBUILDS; i++)
        if (build < 0 && b + g_mv_builds[i][1] == (const unsigned char *)g_s_ui) build = i;   // Steam and GOG: same addresses
    CHECK(build >= 0);
    // that code changed by one byte (mov ecx,[ecx+168h] -> +16Ch, Attila's): refused
    unsigned char *s = b + g_mv_builds[build][0];
    CHECK(VirtualProtect(s, 16, PAGE_EXECUTE_READWRITE, &prot));
    s[14] = 0x6C;
    g_fgp_menu_label = NULL;
    CHECK(install_menuver((HMODULE)b, nolog) == 0 && !g_fgp_menu_label);
    printf("menuver: OK (table entry %d: site +0x%lX, s_ui +0x%lX in %.8s; changed code refused)\n", build,
           g_mv_builds[build][0], rva, (const char *)in->Name);
    return 0;
}
