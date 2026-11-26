// ---- Total War Mod Manager's own parts of the window: the add-on switches and what Play starts.

// What the window shows.
static void host_read_settings(void) {
    char on[MAX_PATH];
    for (int a = 0; a < N_ADDONS; a++) g_addon_disk[a] = addon_on_disk(a);
    joinp(on, sizeof on, g_game, "dinput8.dll");
    g_perf = exists(on);   // off = dinput8.dll.off
    g_up = addon_upscale_on();
    if (!g_addon_disk[AD_UPSCALE]) g_up = 0;
    g_crash_closed = 0;
}

static void ask_open(int kind);   // the questions: picker_ui.inc / below

// ---- "Game files": the game's own files in %APPDATA%\The Creative Assembly\Rome2. 0 = the preferences file (opens in the
// player's editor for .txt), 1 = the scripts folder, 2 = the save folder.
static void game_files(int which) {
    static const char *const sub[3] = {"scripts\\preferences.script.txt", "scripts", "save_games"};
    static const char *const none[3] = {"There is no preferences file yet: the game writes it the first time it runs.",
                                        "There is no scripts folder yet: the game makes it the first time it runs.",
                                        "There are no saves yet."};
    const char *env = getenv("APPDATA");
    char p[MAX_PATH * 2], arg[MAX_PATH * 2 + 4];
    snprintf(p, sizeof p, "%s\\The Creative Assembly\\Rome2\\%s", env ? env : "", sub[which < 0 || which > 2 ? 0 : which]);
    if (!env || !exists(p)) { ui_toast(1, none[which < 0 || which > 2 ? 0 : which]); return; }
    snprintf(arg, sizeof arg, "\"%s\"", p);
    open_explorer(arg);
}

// ---- The questions (picker_ui.inc draws them): CA's launcher this time or from now on, and back from CA's launcher.
// Answer 0 = the first button, 1 = the second, 2 = Cancel / Esc.
static void ask_answer(int kind, int a) {
    g_sheet = SH_NONE;
    SetFocus(g_win);
    if (kind == ASK_CA) {   // 0 = this time only (Enter), 1 = from now on
        if (a == 1) {
            if (launcher_choice_put(2, 0)) lg("rome2fix.ini: launcher=2 (CA's launcher from now on)");
            else ui_toast(2, "Could not write rome2fix.ini: CA's launcher opens this time only.");
        }
        if (a <= 1) host_play(GO_CA);
    } else if (kind == ASK_BACK) {
        if (a == 0) {
            if (launcher_choice_put(1, 0)) ui_toast(0, "Steam's Play opens Total War Mod Manager again (rome2fix.ini: launcher=1).");
            else ui_toast(2, "Could not write rome2fix.ini. Set launcher=1 there by hand.");
        }
    }
    InvalidateRect(g_win, NULL, FALSE);
}

// Play (go = GO_GAME) or the CA's launcher button (GO_CA): everything is saved, the window closes, and WinMain starts it
// and waits.
static void host_play(int go) {
    save_list();
    char on[MAX_PATH], off[MAX_PATH], fb[MAX_PATH];
    joinp(on, sizeof on, g_game, "dinput8.dll");
    joinp(off, sizeof off, g_game, "dinput8.dll.off");
    if (g_perf && !exists(on) && exists(off)) MoveFileExA(off, on, 0);
    else if (!g_perf && exists(on)) MoveFileExA(on, off, MOVEFILE_REPLACE_EXISTING);
    joinp(fb, sizeof fb, g_game, "FrameBridge");
    if (g_addon_disk[AD_UPSCALE] && exists(fb)) { joinp(fb, sizeof fb, g_game, "FrameBridge\\settings.txt"); ini_put(fb, "addon", g_up ? "on" : "off"); }
    g_go = go;
    g_result = 1;
    DestroyWindow(g_win);
}
