// launcher_hook.c in a fake Steam library: steamapps\common\Total War Rome II (Rome2.exe, launcher\launcher.exe, the
// "DLL" = a copy of this exe, run from there) and steamapps\workshop\content\214950\3826423359\TotalWarModManager.exe
// (another copy of this exe as a stand-in: it writes its command line to called.txt in the game folder). Checked:
//   1. CA's launcher.exe (no marker): the Workshop copy runs with --install "<game>", the log says so
//   2. launcher=2 in rome2fix.ini: nothing runs
//   3. launcher.exe already the Mod Manager ("AttilaLauncher-version:100" in it): nothing runs
//   4. no Workshop copy of the item (installed by hand): nothing runs, nothing logged
//   5. the item without TotalWarModManager.exe: nothing runs, one log line
//   6. the stand-in fails (exit 3): the log says CA's launcher stays
#include "tmpdir.h"
#include <string.h>
#ifndef LH_SRC
#define LH_SRC "../launcher_hook.c"
#endif
#include LH_SRC

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAILED line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static char g_log[2048];
static void tlog(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    size_t n = strlen(g_log);
    vsnprintf(g_log + n, sizeof g_log - n, fmt, ap);
    va_end(ap);
    strncat(g_log, "\n", sizeof g_log - strlen(g_log) - 1);
}
static void put(const wchar_t *path, const char *text) {
    FILE *f = _wfopen(path, L"wb");
    if (f) { fputs(text, f); fclose(f); }
}
static int read_called(const wchar_t *game, char *out, size_t n) {   // 1 = the stand-in ran
    wchar_t p[MAX_PATH];
    swprintf(p, MAX_PATH, L"%ls\\called.txt", game);
    FILE *f = _wfopen(p, L"rb");
    out[0] = 0;
    if (!f) return 0;
    size_t k = fread(out, 1, n - 1, f);
    out[k] = 0;
    fclose(f);
    DeleteFileW(p);
    return 1;
}
// runs the copy in the game folder (the "DLL"): it calls rome2_launcher_check with its own module and prints the log
static int run_check(const wchar_t *game_copy, char *log, size_t n) {
    wchar_t cmd[MAX_PATH * 2], out[MAX_PATH];
    swprintf(out, MAX_PATH, L"%ls.log", game_copy);
    swprintf(cmd, MAX_PATH * 2, L"\"%ls\" --check \"%ls\"", game_copy, out);
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(game_copy, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return 0;
    WaitForSingleObject(pi.hProcess, 20000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    FILE *f = _wfopen(out, L"rb");
    log[0] = 0;
    if (f) { size_t k = fread(log, 1, n - 1, f); log[k] = 0; fclose(f); }
    DeleteFileW(out);
    return 1;
}

int wmain(int argc, wchar_t **argv) {
    if (argc >= 3 && !wcscmp(argv[1], L"--check")) {   // the "DLL" in the game folder
        rome2_launcher_check(NULL, tlog);
        FILE *f = _wfopen(argv[2], L"wb");
        if (f) { fputs(g_log, f); fclose(f); }
        return 0;
    }
    if (argc >= 2 && !wcscmp(argv[1], L"--install")) {   // the stand-in Mod Manager: its command line, its working folder
        wchar_t me[MAX_PATH], p[MAX_PATH], cwd[MAX_PATH];
        GetModuleFileNameW(NULL, me, MAX_PATH);
        GetCurrentDirectoryW(MAX_PATH, cwd);
        swprintf(p, MAX_PATH, L"%ls\\called.txt", argc >= 3 ? argv[2] : L".");
        FILE *f = _wfopen(p, L"wb");
        if (f) { fprintf(f, "%ls|%ls", GetCommandLineW(), cwd); fclose(f); }
        swprintf(p, MAX_PATH, L"%ls\\fail.txt", argc >= 3 ? argv[2] : L".");
        return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES ? 3 : 0;
    }
    wchar_t root[MAX_PATH], game[MAX_PATH], ws[MAX_PATH], p[MAX_PATH], me[MAX_PATH], copy[MAX_PATH], mm[MAX_PATH], ini[MAX_PATH];
    tmp_dir(root, L"launcher_hook_test");
    swprintf(p, MAX_PATH, L"%ls\\steamapps", root); CreateDirectoryW(p, NULL);
    swprintf(p, MAX_PATH, L"%ls\\steamapps\\common", root); CreateDirectoryW(p, NULL);
    swprintf(game, MAX_PATH, L"%ls\\steamapps\\common\\Total War Rome II", root); CreateDirectoryW(game, NULL);
    swprintf(p, MAX_PATH, L"%ls\\launcher", game); CreateDirectoryW(p, NULL);
    swprintf(p, MAX_PATH, L"%ls\\Rome2.exe", game); put(p, "MZ game");
    wchar_t ca[MAX_PATH];
    swprintf(ca, MAX_PATH, L"%ls\\launcher\\launcher.exe", game); put(ca, "MZ CA's launcher");
    swprintf(ini, MAX_PATH, L"%ls\\rome2fix.ini", game);
    GetModuleFileNameW(NULL, me, MAX_PATH);
    swprintf(copy, MAX_PATH, L"%ls\\dinput8_stand_in.exe", game);
    CHECK(CopyFileW(me, copy, FALSE));
    char log[2048], called[1024];

    // 4. no Workshop folder for the item
    CHECK(run_check(copy, log, sizeof log));
    CHECK(!read_called(game, called, sizeof called) && !log[0]);
    // 5. the item's folder without the exe
    const wchar_t *parts[] = {L"workshop", L"workshop\\content", L"workshop\\content\\214950", L"workshop\\content\\214950\\3826423359"};
    for (int i = 0; i < 4; i++) { swprintf(p, MAX_PATH, L"%ls\\steamapps\\%ls", root, parts[i]); CreateDirectoryW(p, NULL); }
    swprintf(ws, MAX_PATH, L"%ls\\steamapps\\workshop\\content\\214950\\3826423359", root);
    CHECK(run_check(copy, log, sizeof log));
    CHECK(!read_called(game, called, sizeof called) && strstr(log, "has no TotalWarModManager.exe"));
    // 1. CA's launcher in place: the Workshop copy is run with --install "<game>", in the game folder
    swprintf(mm, MAX_PATH, L"%ls\\TotalWarModManager.exe", ws);
    CHECK(CopyFileW(me, mm, FALSE));
    CHECK(run_check(copy, log, sizeof log));
    int ran = read_called(game, called, sizeof called);
    char want[MAX_PATH * 3];
    snprintf(want, sizeof want, "\"%ls\" --install \"%ls\"|%ls", mm, game, game);
    printf("1. ran: %d, its command line|folder: %s\n   log: %s", ran, called, log);
    CHECK(ran && !strcmp(called, want) && strstr(log, "now opens Total War Mod Manager"));
    // 6. the install fails: CA's launcher stays
    swprintf(p, MAX_PATH, L"%ls\\fail.txt", game); put(p, "x");
    CHECK(run_check(copy, log, sizeof log));
    CHECK(read_called(game, called, sizeof called) && strstr(log, "failed (code 3); CA's launcher stays"));
    DeleteFileW(p);
    // 2. launcher=2: the player's choice
    put(ini, "[mod]\r\nlauncher=2\r\n");
    CHECK(run_check(copy, log, sizeof log));
    CHECK(!read_called(game, called, sizeof called) && !log[0]);
    put(ini, "[mod]\r\nlauncher=1\r\n");
    CHECK(run_check(copy, log, sizeof log));
    CHECK(read_called(game, called, sizeof called));
    // 3. the Mod Manager already in launcher.exe
    put(ca, "MZ ... AttilaLauncher-version:100 ...");
    CHECK(run_check(copy, log, sizeof log));
    CHECK(!read_called(game, called, sizeof called) && !log[0]);
    // not Rome 2's folder (no Rome2.exe next to the DLL): nothing
    put(ca, "MZ CA's launcher");
    swprintf(p, MAX_PATH, L"%ls\\Rome2.exe", game); DeleteFileW(p);
    CHECK(run_check(copy, log, sizeof log));
    CHECK(!read_called(game, called, sizeof called) && !log[0]);

    puts(fails ? "launcher_hook test FAILED" : "launcher_hook test passed");
    return fails != 0;
}
