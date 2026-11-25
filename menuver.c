// Main menu: "Engine Fix <version>" / "by JGdaCaesar" in the top right while the game's front end (its menus) is up,
// drawn by the panel (fgpanel.c, g_fgp_menu_label) on the game's frames (from Attila 7.4's menuver.c).
// "The menus are up" is the front end's own pointer (Attila: UIDLL::FRONT_END_UI::s_ui; Rome 2 names no class, the code
// is the same shape): set to `this` by its constructor, cleared by its destructor, read by the main menu's OnCreate
// right after it looks up the "version_number" box (with "button_start_campaign", "button_steam_store", "theme_icon"):
//   Steam / GOG: ctor +0x1077A40 (s_ui = ebp at +0x1077DA4), dtor +0x1079FD0 (s_ui = 0 at +0x107A072), OnCreate +0x106E3E0
//   pantheon:    ctor +0x1052E60 (+0x10531C4),               dtor +0x10553E0 (+0x1055482),             OnCreate +0x10497D0
// (capstone 2026-11-23: the only two writes to that pointer in each build; ctor and dtor store the same vtable.)
// Its address is read from OnCreate's `mov ecx,[s_ui]` after that code is checked byte for byte, so it is right on any
// load address.
#include <windows.h>
#include <string.h>
#include "builds.h"
#include "version.h"

extern int (*g_fgp_menu_label)(void);   // fgpanel.c
static void *const volatile *g_s_ui;

// per known build (builds.h order): OnCreate's `mov ecx,[s_ui]`, s_ui's RVA
static const DWORD g_mv_builds[NBUILDS][2] = {
    {0x106E410, 0x3FB2C94},   // Steam 69CE2041
    {0x1049800, 0x201D564},   // pantheon beta 6AB978BD
    {0x106E410, 0x3FB2C94},   // GOG 6A564E92
};

static int menus_up(void) { return *g_s_ui != NULL; }

// the s_ui pointer of m's build (checked code), or NULL
static void *const volatile *mv_find(const unsigned char *b) {
    // push "version_number" (-0x17) ... mov ecx,[s_ui]; mov esi,eax; lea eax,[esp+2Ch]; mov ecx,[ecx+168h];
    // mov ecx,[ecx+38h]; add ecx,48h; push ecx; push eax; call
    static const unsigned char after[21] = {0x8B, 0xF0, 0x8D, 0x44, 0x24, 0x2C, 0x8B, 0x89, 0x68, 0x01, 0x00,
                                            0x00, 0x8B, 0x49, 0x38, 0x83, 0xC1, 0x48, 0x51, 0x50, 0xE8};
    for (int i = 0; i < NBUILDS; i++) {
        DWORD site = g_mv_builds[i][0], s_ui = g_mv_builds[i][1], at, name;
        if (!in_image(b, site - 0x17, 0x17 + 6 + sizeof after) || !in_image(b, s_ui, 4)) continue;
        const unsigned char *s = b + site;
        memcpy(&at, s + 2, 4);
        memcpy(&name, s - 0x16, 4);
        if (s[0] != 0x8B || s[1] != 0x0D || memcmp(s + 6, after, sizeof after) || at != (DWORD)(UINT_PTR)(b + s_ui) ||
            s[-0x17] != 0x68 || name - (DWORD)(UINT_PTR)b >= 0x10000000u || !in_image(b, name - (DWORD)(UINT_PTR)b, 15) ||
            memcmp((const char *)(UINT_PTR)name, "version_number", 15))
            continue;
        return (void *const volatile *)(UINT_PTR)at;
    }
    return NULL;
}

int install_menuver(HMODULE m, void (*log)(const char *fmt, ...)) {
    void *const volatile *p = mv_find((const unsigned char *)m);
    MEMORY_BASIC_INFORMATION mi;
    if (!p || !VirtualQuery((const void *)p, &mi, sizeof mi) || mi.State != MEM_COMMIT || mi.AllocationBase != (void *)m) {
        log("main menu label: the game's main menu code differs from the known game versions - left out");
        return 0;
    }
    g_s_ui = p;
    g_fgp_menu_label = menus_up;
    log("main menu: \"Engine Fix " MOD_VERSION_STR "\" in the top right while the menus are up");
    return 1;
}
