// dinput8.dll proxy for Total War: ROME 2 (empire.retail.dll of 2026-09-17).
// empire.retail.dll imports DINPUT8.dll, so this loads before the game's own startup code runs.
// It installs the fixes that profiling showed matter (see AGENT_LOG.md) and forwards the real
// DirectInput exports to the system dinput8.dll. Log: rome2fix.log next to this DLL.
// Built with -DPROFILING it also has the measuring tools (probes.txt, fps_log.txt, tilescan_ab.txt,
// sortfix_ab.txt); the release build has only the fixes, the off switches, diagnostics and the self-update.
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "version.h"
#include "logtrim.h"

int install_tilescan(HMODULE m, int mode, int fpslog, void (*log)(const char *fmt, ...));  // tilescan.c
extern void (*g_frame_check)(void);                                                    // tilescan.c
int install_memwatch(HMODULE m, void (*trim)(void), void (*log)(const char *fmt, ...));  // memwatch.c
int install_scriptext(HMODULE m, const wchar_t *dir, void (*log)(const char *fmt, ...));  // scriptext.c
void scriptext_frame(void);                                                             // scriptext.c
int load_plugins(const wchar_t *dir, void (*log)(const char *fmt, ...));               // plugins.c
int install_fpslog(const wchar_t *dir);                                                 // fpslog.c
static int g_scriptext_on, g_late_install;   // late: install_all runs from the DLL load notification, not DllMain
int install_x87fix(HMODULE m, void (*log)(const char *fmt, ...));                      // x87fix.c (from the Attila mod)
void x87fix_frame(void);                                                               // x87fix.c
int install_sortfix(HMODULE m, void (*log)(const char *fmt, ...));                     // sortfix.c (from the Attila mod)
void diag_startup(HMODULE empire, const wchar_t *log_path, void (*log)(const char *fmt, ...));  // diag.c
extern int g_crash_logging;                                                                      // diag.c
void update_check(HMODULE self, void (*log)(const char *fmt, ...));                             // update.c
int install_fileio(HMODULE m, void (*log)(const char *fmt, ...));                              // fileio.c
int install_strfix(HMODULE m, void (*log)(const char *fmt, ...));                              // strfix.c
int install_soundguard(HMODULE m, void (*log)(const char *fmt, ...));                          // soundguard.c
int install_parsort(HMODULE m, int ab, void (*log)(const char *fmt, ...));                     // parsort.c
int install_uicache(HMODULE m, const char *names, void *test_draw, void (*log)(const char *fmt, ...));  // uicache.c
int install_attrcache(HMODULE m, int verify, void (*log)(const char *fmt, ...));               // attrcache.c
int install_bridgeprobe(HMODULE m, const wchar_t *dir, void (*log)(const char *fmt, ...));     // bridgeprobe.c (the DLSS add-on)
int install_overlay(HMODULE m, void (*log)(const char *fmt, ...));                             // overlay.c: F8 without the add-on
int install_menuver(HMODULE m, void (*log)(const char *fmt, ...));                             // menuver.c
#ifdef PROFILING
// the menu-picture fix is profiling-only: it halves the reloads but made no measurable difference (AGENT_LOG 2026-10-27)
int install_uikeep(HMODULE m, int loadnames, void (*log)(const char *fmt, ...));               // uikeep.c
void sortfix_enable(int on);                                                         // sortfix.c
int install_probes(HMODULE m, const wchar_t *list, const wchar_t *out, void (*log)(const char *fmt, ...));  // probes.c
void probes_frame(void);                                                               // probes.c
int install_viewtouch(HMODULE m, const wchar_t *cfg, void (*log)(const char *fmt, ...));  // viewtouch.c
int install_attrprobe(HMODULE m, void (*log)(const char *fmt, ...));                      // attrprobe.c
int install_heapfix(HMODULE m, void (*log)(const char *fmt, ...));                        // heapfix.c
extern void (*g_ab_flip)(int on);                                                      // tilescan.c
extern const char *g_ab_name;
static int g_probes_on;
extern volatile unsigned char g_uikeep_on, g_ac_open_on;  // uikeep.c, attrcache.c
static void menus_enable(int on) { g_uikeep_on = g_ac_open_on = (unsigned char)on; }
#endif

static int g_x87_on;
static HMODULE g_self;

// Per frame on the main thread (from the tile-scan entry hook).
static void frame_checks(void) {
    if (g_x87_on) x87fix_frame();
    if (g_scriptext_on) scriptext_frame();
#ifdef PROFILING
    if (g_probes_on) probes_frame();
#endif
}

static wchar_t g_log_path[MAX_PATH];

// One line per game start that stands out in the log: version and date.
static void log_header(void) {
    trim_log(g_log_path);  // past 1 MB, the oldest sessions go (players must be able to post it)
    FILE *f = _wfopen(g_log_path, L"a");
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "\n======== Rome 2 Engine Performance Fix v%s - game started %04d-%02d-%02d %02d:%02d ========\n",
            MOD_VERSION_STR, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    fclose(f);
}

static void wlog(const char *fmt, ...) {
    if (!g_log_path[0]) return;
    char buf[4096];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = snprintf(buf, sizeof buf, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a;
    va_start(a, fmt);
    int m = vsnprintf(buf + n, sizeof buf - n - 2, fmt, a);
    va_end(a);
    n += m < 0 ? 0 : m < (int)sizeof buf - n - 2 ? m : (int)sizeof buf - n - 3;   // a longer line is cut
    buf[n++] = '\r'; buf[n++] = '\n';   // as the text-mode file had it
    log_append(g_log_path, buf, n);     // logtrim.h: one atomic append, no file open per line (Attila round 2)
}

// ---- dinput8 forwarding (the real DLL is loaded on first use, never inside DllMain)
static HMODULE g_real;

static FARPROC real_proc(const char *name) {
    if (!g_real) {
        wchar_t path[MAX_PATH];
        UINT n = GetSystemDirectoryW(path, MAX_PATH);  // SysWOW64 for this 32-bit game
        if (n == 0 || n > MAX_PATH - 14) return NULL;
        wcscat(path, L"\\dinput8.dll");
        g_real = LoadLibraryW(path);
        if (!g_real) return NULL;
    }
    return GetProcAddress(g_real, name);
}

void (*g_dev_di8_created)(void *di);   // the DLSS add-on's F11 panel filters the game's mouse while open (fgpanel.c)
HRESULT WINAPI stub_DirectInput8Create(HINSTANCE h, DWORD v, REFIID r, LPVOID *o, LPUNKNOWN u) {
    typedef HRESULT(WINAPI * fn)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
    fn f = (fn)real_proc("DirectInput8Create");
    if (f && g_dev_di8_created) {
        HRESULT hr = f(h, v, r, o, u);
        if (SUCCEEDED(hr) && o && *o) g_dev_di8_created(*o);
        return hr;
    }
    return f ? f(h, v, r, o, u) : E_FAIL;
}
HRESULT WINAPI stub_DllCanUnloadNow(void) {
    typedef HRESULT(WINAPI * fn)(void);
    fn f = (fn)real_proc("DllCanUnloadNow");
    return f ? f() : S_FALSE;
}
HRESULT WINAPI stub_DllGetClassObject(REFCLSID c, REFIID r, LPVOID *o) {
    typedef HRESULT(WINAPI * fn)(REFCLSID, REFIID, LPVOID *);
    fn f = (fn)real_proc("DllGetClassObject");
    return f ? f(c, r, o) : E_FAIL;
}
HRESULT WINAPI stub_DllRegisterServer(void) {
    typedef HRESULT(WINAPI * fn)(void);
    fn f = (fn)real_proc("DllRegisterServer");
    return f ? f() : E_FAIL;
}
HRESULT WINAPI stub_DllUnregisterServer(void) {
    typedef HRESULT(WINAPI * fn)(void);
    fn f = (fn)real_proc("DllUnregisterServer");
    return f ? f() : E_FAIL;
}

// The plain-language summary after the install lines (those stay as the details for bug reports).
enum { FIX_SWITCHED_OFF = -1, FIX_INACTIVE = 0, FIX_ON = 1, FIX_PARTLY = 2 };
#ifdef PROFILING
#define NFIX 9
#else
#define NFIX 8  // without "Menus" (profiling-only)
#endif
static const char *const g_fix_names[9] = {"FPS: tile feedback scan", "FPS: x87 rounding", "FPS: render-list sorts",
                                           "Loading: file reading", "Loading: string functions", "Loading: missing-file cache",
                                           "Stability: sound crash guard (huge battles)", "FPS: battle UI (unit cards drawn from a cache)",
                                           "Menus: pictures stay loaded"};

static void log_status(const int st[9]) {
    int on = 0;
    for (int i = 0; i < NFIX; i++) on += st[i] == FIX_ON;
    if (on == NFIX)
        wlog("STATUS: all %d fixes are on", NFIX);
    else
        wlog("STATUS: %d of %d fixes are on", on, NFIX);
    for (int i = 0; i < NFIX; i++)
        wlog("  %-4s %s%s", st[i] == FIX_ON ? "on" : st[i] == FIX_PARTLY ? "part" : "OFF", g_fix_names[i],
             st[i] == FIX_SWITCHED_OFF ? " (switched off in rome2fix.ini, or by its _off.txt file)"
             : st[i] == FIX_PARTLY ? " (only part of it fits this game version)"
             : st[i] == FIX_INACTIVE ? " (does not fit this game version, so it stays off; details above)" : "");
}

// rome2fix.ini in the game folder: written once with the defaults when missing; after that the mod changes only
// [tuning] culling= (the F8 panel's pick).
static const char g_ini_defaults[] =
    "; Rome 2 Engine Performance Fix settings. 1 = on, 0 = off. Restart the game after a change.\r\n"
    "; The mod writes this file with the defaults and adds the lines a newer version brings; otherwise it changes only\r\n"
    "; culling= (your pick in the F8 panel).\r\n"
    "; Delete it to get the defaults back.\r\n"
    "; (The older way still works too: an empty <name>_off.txt file in the game folder switches that fix off.)\r\n"
    "\r\n"
    "[fixes]\r\n"
    "; FPS\r\n"
    "tile_feedback_scan=1\r\n"
    "x87_rounding=1\r\n"
    "render_list_sorts=1\r\n"
    "parallel_sort=1\r\n"
    "; loading\r\n"
    "file_reading=1\r\n"
    "string_functions=1\r\n"
    "missing_file_cache=1\r\n"
    "; stability\r\n"
    "sound_crash_guard=1\r\n"
    "; FPS in battles: the unit cards shown from a cache, redrawn a few times a second and on input\r\n"
    "battle_ui_cache=1\r\n"
    "\r\n"
    "[mod]\r\n"
    "; copy a newer version from the Steam Workshop into the game folder at startup\r\n"
    "self_update=1\r\n"
    "; record crashes in rome2fix.log (EXCEPTION lines, a crash_*.dmp file for the first one)\r\n"
    "crash_logging=1\r\n"
    "; load other mods' DLLs from the plugins folder in the game folder (any name, e.g. plugins\\smoke.dll)\r\n"
    "plugins=1\r\n"
    "\r\n"
    "[tuning]\r\n"
    "; Culling: small parts of far-away soldiers (belts, quivers, bridles) are not drawn, for more fps in big battles:\r\n"
    "; off, light, normal or strong (also in the F8 panel, which saves its pick here)\r\n"
    "culling=light\r\n"
    "\r\n"
    "; The DLSS add-on has its own settings: FrameBridge\\settings.txt (F11 in game).\r\n";

static void ini_write_defaults(const wchar_t *path) {
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);  // only when missing
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(h, g_ini_defaults, sizeof g_ini_defaults - 1, &w, NULL);
    CloseHandle(h);
}

// The defaults' lines an existing rome2fix.ini does not have are added to it, each with the comment lines right above
// it in the defaults, at the end of its section (a missing section at the end of the file). The user's own lines and
// values stay. The older header lines, which say the file is never changed, are replaced.
// shortcut: the replacements match Windows line ends only (as the mod and Notepad write them); a file with bare \n
// ones still gets its missing lines
static const char *const g_ini_replace[][2] = {
    {"; The mod writes this file once with the defaults and never changes it. Delete it to get the defaults back.\r\n",
     "; The mod writes this file with the defaults and adds the lines a newer version brings; otherwise it changes only\r\n"
     "; culling= (your pick in the F8 panel). Delete it to get the defaults back.\r\n"},
    {"; The mod writes this file once with the defaults; after that it changes only culling= (your pick in the F8 panel).\r\n",
     "; The mod writes this file with the defaults and adds the lines a newer version brings; otherwise it changes only\r\n"
     "; culling= (your pick in the F8 panel).\r\n"}};
enum { INI_MAX = 65536 };
static const char *ini_next(const char *p) {
    while (*p && *p != '\n') p++;
    return *p ? p + 1 : p;
}
// the line at p is "[sec]" / "key =..." (case ignored)
static int ini_is_sec(const char *p, const char *sec, size_t n) { return p[0] == '[' && !_strnicmp(p + 1, sec, n) && p[n + 1] == ']'; }
static int ini_is_key(const char *p, const char *key, size_t n) {
    if (_strnicmp(p, key, n)) return 0;
    for (p += n; *p == ' ' || *p == '\t'; p++) {}
    return *p == '=';
}
// n bytes at buf + at, the rest moved up; 0 when it does not fit
static int ini_insert(char *buf, size_t *len, size_t at, const char *add, size_t n) {
    if (*len + n >= INI_MAX) return 0;
    memmove(buf + at + n, buf + at, *len - at + 1);
    memcpy(buf + at, add, n);
    *len += n;
    return 1;
}
static void ini_update(const wchar_t *path) {
    static char buf[INI_MAX];
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD n = 0;
    BOOL ok = ReadFile(h, buf, INI_MAX - 1, &n, NULL) && n < INI_MAX - 1;   // a file this big is left alone
    CloseHandle(h);
    if (!ok) return;
    buf[n] = 0;
    size_t len = n;
    int changed = 0;
    for (unsigned r = 0; r < sizeof g_ini_replace / sizeof g_ini_replace[0]; r++) {
        char *at = strstr(buf, g_ini_replace[r][0]);
        size_t a = strlen(g_ini_replace[r][0]), b = strlen(g_ini_replace[r][1]);
        if (!at || len - a + b >= INI_MAX) continue;
        memmove(at + b, at + a, len - (size_t)(at - buf) - a + 1);
        memcpy(at, g_ini_replace[r][1], b);
        len = len - a + b;
        changed = 1;
    }
    char sec[32] = "";
    const char *cmt = NULL;   // the first of the comment lines right above the current line of the defaults
    for (const char *d = g_ini_defaults; *d; d = ini_next(d)) {
        const char *e = ini_next(d), *eq = memchr(d, '=', (size_t)(e - d));
        if (*d == ';') {
            if (!cmt) cmt = d;
            continue;
        }
        if (*d == '[') {
            const char *c = memchr(d, ']', (size_t)(e - d));
            size_t sn = c ? (size_t)(c - d - 1) : 0;
            if (sn < sizeof sec) { memcpy(sec, d + 1, sn); sec[sn] = 0; }
        } else if (eq && sec[0]) {
            size_t sn = strlen(sec), kn = (size_t)(eq - d);
            const char *s0 = NULL, *s1 = buf + len, *last = NULL;   // the section in the file: after its header, the next header
            for (const char *p = buf; *p; p = ini_next(p)) {
                if (s0 && *p == '[') { s1 = p; break; }
                if (!s0 && ini_is_sec(p, sec, sn)) s0 = ini_next(p);
            }
            int have = 0;
            for (const char *p = s0; p && p < s1 && !have; p = ini_next(p)) have = ini_is_key(p, d, kn);
            if (!have) {
                for (const char *p = s0; p && p < s1; p = ini_next(p))
                    if (*p != '\r' && *p != '\n' && *p != ';') last = ini_next(p);   // after the section's last key line
                const char *from = cmt ? cmt : d;
                size_t at = s0 ? (size_t)((last ? last : s0) - buf) : len;
                if (at && buf[at - 1] != '\n' && ini_insert(buf, &len, at, "\r\n", 2)) at += 2, changed = 1;   // a last line without its line end
                if (!s0) {
                    char head[40];
                    int hn = snprintf(head, sizeof head, "%s[%s]\r\n", len ? "\r\n" : "", sec);
                    if (!ini_insert(buf, &len, at, head, (size_t)hn)) break;
                    at += (size_t)hn;
                    changed = 1;
                }
                changed |= ini_insert(buf, &len, at, from, (size_t)(e - from));
            }
        }
        cmt = NULL;
    }
    if (!changed) return;
    // written beside it first, then put in its place: the old file stays whole if writing fails
    wchar_t tmp[MAX_PATH + 8];
    swprintf(tmp, MAX_PATH + 8, L"%ls.new", path);
    h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    ok = WriteFile(h, buf, (DWORD)len, &w, NULL) && w == len;
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp);
}

// An ini switch: off only for 0 / off / false / no (GetPrivateProfileInt read "on", "yes", "true" as 0 = off); missing,
// empty or anything else = on, the default.
int ini_on(const wchar_t *sec, const wchar_t *key, const wchar_t *ini) {
    wchar_t v[16];
    GetPrivateProfileStringW(sec, key, L"1", v, 16, ini);
    return _wcsicmp(v, L"0") && _wcsicmp(v, L"off") && _wcsicmp(v, L"false") && _wcsicmp(v, L"no");
}

// Culling (partcull.c, from Attila 7.1): Off / Light / Normal / Strong, from rome2fix.ini [tuning] culling=; the F8
// panel's pick is saved there. The threshold per choice is the panel's (fgpanel.c g_fgp_cull_px). Light by default
// (user, 2026-11-25), as Attila ships it; the part box offset is read from the code, not yet checked in game.
int install_partcull(HMODULE m, void (*log)(const char *fmt, ...));                            // partcull.c
void partcull_counts(unsigned *parts, unsigned *skipped);
unsigned partcull_forced(void);
extern volatile LONG g_partcull_on;
extern float g_partcull_px;
extern const float g_fgp_cull_px[4];
void fgp_set_cull(int choice), fgp_set_cull_available(int ok);
void fgp_set_cull_source(void (*counts)(unsigned *, unsigned *, unsigned *), void (*apply)(int));
static const wchar_t *const CULL_KEYS[4] = {L"off", L"light", L"normal", L"strong"};
static int g_culling, g_culling_ok;
static wchar_t g_ini[MAX_PATH];
static void culling_choose(int c) {
    g_culling = c >= 0 && c < 4 ? c : 0;
    if (g_culling) g_partcull_px = g_fgp_cull_px[g_culling];
    InterlockedExchange(&g_partcull_on, g_culling > 0);
}
static void culling_pick(int c) {   // from the F8 panel: applied now and saved
    culling_choose(c);
    if (g_ini[0]) WritePrivateProfileStringW(L"tuning", L"culling", CULL_KEYS[g_culling], g_ini);
    wlog("culling: %ls (picked in the F8 panel, saved in rome2fix.ini)", CULL_KEYS[g_culling]);
}
static void culling_counts(unsigned *parts, unsigned *skipped, unsigned *forced) {
    partcull_counts(parts, skipped);
    *forced = partcull_forced();
}
// the [tuning] culling= value: off / light / normal / strong or 0..3; anything else is off
static int culling_from_ini(const wchar_t *v) {
    int c = v[0] >= L'0' && v[0] <= L'3' && !v[1] ? v[0] - L'0' : 0;
    for (int i = 1; i < 4; i++) if (!_wcsicmp(v, CULL_KEYS[i])) c = i;
    return c;
}

// Installs everything into the game DLL m (mapped and relocated, its own startup code not run yet).
static void install_all(HMODULE m) {
    int st[9] = {0};
    // Switches: rome2fix.ini, or (older) empty <name>_off.txt files in the game folder.
    wchar_t f[MAX_PATH], ini[MAX_PATH];
    wchar_t *name = wcsrchr(g_log_path, L'\\') - g_log_path + f + 1;
    wcscpy(f, g_log_path);
#define PRESENT(n) (wcscpy(name, n), GetFileAttributesW(f) != INVALID_FILE_ATTRIBUTES)
    wcscpy(ini, f);
    wcscpy(ini + (name - f), L"rome2fix.ini");
    ini_write_defaults(ini);
    ini_update(ini);
    wcscpy(g_ini, ini);
#define INI(sec, key) ini_on(sec, key, ini)
#define OFF(key, file) (!INI(L"fixes", key) || PRESENT(file))
    g_crash_logging = INI(L"mod", L"crash_logging");
    diag_startup(m, g_log_path, wlog);  // version, process, game build, system, settings; crash records
    if (g_crash_logging) install_memwatch(m, NULL, wlog);   // failed file maps / allocations and low address space logged
    if (INI(L"mod", L"self_update"))
        update_check(g_self, wlog);     // a newer Workshop copy replaces this DLL for the next launch
    else
        wlog("self-update off (self_update=0 in rome2fix.ini)");  // also for off / false / no
    {   // the pop-ups after the game starts (fgpanel.c, with the DLSS add-on): a newer version installed, and What's new on
        // the first start of this one (the last version started is in rome2fix_seen.txt)
        extern int g_update_state;   // update.c
        void fgp_set_update(int state), fgp_show_whatsnew(void);
        fgp_set_update(g_update_state);
        char t[16] = {0};
        FILE *sf = (wcscpy(name, L"rome2fix_seen.txt"), _wfopen(f, L"rb"));
        if (sf) { fread(t, 1, sizeof t - 1, sf); fclose(sf); }
        if (atoi(t) != MOD_VERSION_NUM) {
            fgp_show_whatsnew();
            if ((sf = _wfopen(f, L"wb"))) { fprintf(sf, "%d\n", MOD_VERSION_NUM); fclose(sf); }
        }
    }
#ifdef PROFILING
    // Heap test (opt-in, heapfix_on.txt): must go in before the game's first allocation, which is still to come here.
    // mimalloc frees a thread's state when the thread ends, so thread notifications stay on while it is installed.
    if (!(PRESENT(L"heapfix_on.txt") && install_heapfix(m, wlog) > 0)) DisableThreadLibraryCalls(g_self);
#endif
    int tile_mode = OFF(L"tile_feedback_scan", L"tilescan_off.txt") ? 0 : 1, fpslog = 0;
#ifdef PROFILING
    if (tile_mode && PRESENT(L"tilescan_ab.txt")) tile_mode = 2;
    fpslog = PRESENT(L"fps_log.txt");
#endif
    // The entry hook of the tile scan is also the per-frame callback the x87 fix needs for its safety check.
    int ts = install_tilescan(m, tile_mode, fpslog, wlog);
    wchar_t dir[MAX_PATH];   // the game folder
    wcscpy(dir, f);
    dir[name - f - 1] = 0;
    if (ts && install_fpslog(dir)) wlog("frame times to fps_log.csv (fps_on.txt found; delete it to stop)");
    // script extender: only with a lua_ext folder in the game folder (scriptext.c); runs from the frame hook
    if (ts) g_scriptext_on = install_scriptext(m, dir, wlog);
    st[0] = !ts ? FIX_INACTIVE : tile_mode ? FIX_ON : FIX_SWITCHED_OFF;
    // Battles: a soldier's small parts are not drawn once they would be a few pixels tall (partcull.c); the choice is
    // rome2fix.ini [tuning] culling= or the F8 panel; partcull_off.txt leaves it out entirely.
    // Before the x87 fix: that patches a rounding block inside add_instance, which partcull checks byte for byte.
    if (PRESENT(L"partcull_off.txt")) wlog("culling not installed (partcull_off.txt)");
    else g_culling_ok = install_partcull(m, wlog) > 0;
    if (g_culling_ok) {
        wchar_t v[16] = L"";
        GetPrivateProfileStringW(L"tuning", L"culling", L"light", v, 16, ini);
        culling_choose(culling_from_ini(v));
    }
    fgp_set_cull_source(culling_counts, culling_pick);
    fgp_set_cull_available(g_culling_ok);
    fgp_set_cull(g_culling);
    // Battles: the unit cards are not walked and drawn every frame but shown from a cache that is redrawn a few times a
    // second and on input (uicache.c, from Attila 7.1). It counts frames with g_frame_no: only with the frame hook in.
    // Before the x87 fix too: that patches rounding blocks inside the UI's Draw, which this checks byte for byte.
    if (OFF(L"battle_ui_cache", L"uicache_off.txt")) {
        wlog("battle UI cache switched off (rome2fix.ini or uicache_off.txt) - the unit cards are drawn every frame, as the game does");
        st[7] = FIX_SWITCHED_OFF;
    } else if (!ts)
        wlog("battle UI cache not installed (needs the frame hook)");
    else
        st[7] = install_uicache(m, NULL, NULL, wlog) > 0;
    if (!ts)
        wlog("x87 fix not installed (needs the frame hook)");
    else if (OFF(L"x87_rounding", L"x87fix_off.txt")) {
        wlog("x87 rounding fix switched off (rome2fix.ini or x87fix_off.txt) - not installed");
        st[1] = FIX_SWITCHED_OFF;
    } else
        st[1] = (g_x87_on = install_x87fix(m, wlog) > 0);
    g_frame_check = frame_checks;
    if (OFF(L"file_reading", L"fileio_off.txt")) {
        wlog("file I/O fix switched off (rome2fix.ini or fileio_off.txt) - not installed");
        st[3] = FIX_SWITCHED_OFF;
    } else
        st[3] = install_fileio(m, wlog) > 0;
    if (OFF(L"string_functions", L"strfix_off.txt")) {
        wlog("string functions switched off (rome2fix.ini or strfix_off.txt) - not replaced");
        st[4] = FIX_SWITCHED_OFF;
    } else
        st[4] = install_strfix(m, wlog) > 0;
    if (OFF(L"missing_file_cache", L"attrcache_off.txt")) {
        wlog("missing-file cache switched off (rome2fix.ini or attrcache_off.txt) - not installed");
        st[5] = FIX_SWITCHED_OFF;
    } else {
        int verify = 0;
#ifdef PROFILING
        verify = PRESENT(L"attrcache_verify.txt");  // answer from Windows, count where the cache would differ
#endif
        st[5] = install_attrcache(m, verify, wlog) > 0;
    }
    // Huge battles: a sound the game's full sound pool gave no buffer is skipped instead of crashing (soundguard.c)
    if (OFF(L"sound_crash_guard", L"soundguard_off.txt")) {
        wlog("sound crash guard switched off (rome2fix.ini or soundguard_off.txt) - not installed");
        st[6] = FIX_SWITCHED_OFF;
    } else
        st[6] = install_soundguard(m, wlog) > 0;
#ifdef PROFILING
    int loadnames = PRESENT(L"loadnames_on.txt");  // count menu picture loads per name
    if (PRESENT(L"uikeep_off.txt")) {
        wlog("uikeep_off.txt found - menu pictures freed when closed, as the game does");
        st[8] = FIX_SWITCHED_OFF;
    } else
        st[8] = install_uikeep(m, loadnames, wlog) > 0;
#endif
    int sorts = 0;
    if (OFF(L"render_list_sorts", L"sortfix_off.txt")) {
        wlog("render-list sorts switched off (rome2fix.ini or sortfix_off.txt) - not replaced");
        st[2] = FIX_SWITCHED_OFF;
    } else
        sorts = install_sortfix(m, wlog);
    // Battles: the soldiers' draw lists sorted on the main thread + 3 helpers (parsort.c, from Attila 7.4; Rome 2 has no
    // list job, the main thread does that work itself). Part of the sorts: no status line of its own; needs sortfix's
    // draw-list sort. parsort_ab.txt: 10 s windows on / off with the block's time in the log (the in-game A/B).
    if (sorts && OFF(L"parallel_sort", L"parsort_off.txt"))
        wlog("soldier draw lists on several threads switched off (rome2fix.ini or parsort_off.txt) - sorted on one thread, as before");
    else if (sorts)
        install_parsort(m, PRESENT(L"parsort_ab.txt"), wlog);
    if (st[2] != FIX_SWITCHED_OFF) st[2] = sorts == 2 ? FIX_ON : sorts ? FIX_PARTLY : FIX_INACTIVE;
    log_status(st);
    wlog("  Culling: %ls%s", g_culling_ok ? CULL_KEYS[g_culling] : L"not available",
         g_culling_ok ? " (optional; F8 panel or rome2fix.ini [tuning] culling=)" : "");
#ifdef PROFILING
    if (sorts == 2 && PRESENT(L"sortfix_ab.txt")) {
        // A/B: the sorts flip on/off every 10 s at frame start, fps logged per window.
        g_ab_name = "sortfix";
        g_ab_flip = sortfix_enable;
        wlog("sortfix_ab.txt found - A/B mode for the sort fix");
    } else if (st[8] == FIX_ON && PRESENT(L"uikeep_ab.txt")) {
        // A/B: menu pictures kept + menu stat opens cached, on/off every 10 s (off frees everything kept)
        g_ab_name = "menus";
        g_ab_flip = menus_enable;
        wlog("uikeep_ab.txt found - A/B mode for the menu fixes");
    }
    // probes.txt: time the listed functions per frame into probes_<date>_<time>.bin.
    if (PRESENT(L"probes.txt")) {
        wchar_t list[MAX_PATH], out[MAX_PATH];
        wcscpy(list, f);
        SYSTEMTIME t;
        GetLocalTime(&t);
        wcscpy(out, f);
        swprintf(out + (name - f), MAX_PATH - (name - f), L"probes_%04d%02d%02d_%02d%02d%02d.bin", t.wYear, t.wMonth, t.wDay,
                 t.wHour, t.wMinute, t.wSecond);
        g_probes_on = install_probes(m, list, out, wlog) > 0;
    }
    // viewtouch.txt: loading experiment on the page touch after each file view is mapped (viewtouch.c)
    if (PRESENT(L"viewtouch.txt")) install_viewtouch(m, f, wlog);
    // attrprobe.txt: count and time the game's GetFileAttributesW calls (attrprobe.c)
    if (PRESENT(L"attrprobe.txt")) install_attrprobe(m, wlog);
#endif
    // the DLSS add-on (Upscaling and Framegen for Rome II): <game>\FrameBridge\ with the 64-bit helper and NVIDIA's DLLs;
    // without it, or with addon=off in its settings.txt, nothing is installed. Dev builds: also bridgeprobe.txt (the
    // bridge capture probe, fgbridge_on.txt; dlss/DLSS_LOG.md)
    int addon = PRESENT(L"FrameBridge\\helper64.exe");
    if (addon) {
        char text[4096] = {0};   // the add-on's settings.txt is past 1 KB (addon= at byte 1486 in the installed one)
        FILE *sf = PRESENT(L"FrameBridge\\settings.txt") ? _wfopen(f, L"r") : NULL;
        if (sf) { fread(text, 1, sizeof text - 1, sf); fclose(sf); }
        if (strstr(text, "\naddon=off") || !strncmp(text, "addon=off", 9)) {
            wlog("DLSS add-on found but switched off (addon=off in FrameBridge\\settings.txt) - the game runs with the performance fix only");
            addon = 0;
        }
    }
#ifdef PROFILING
    if (PRESENT(L"bridgeprobe.txt")) addon = 1;
#endif
    // without the add-on the F8 panel (Culling, What's new) and the pop-ups run on the game's own frames (overlay.c);
    // with it the bridge draws them, so the overlay never goes in beside it (both wrap the same GetProcAddress import)
    if (addon) install_bridgeprobe(m, dir, wlog);
    else install_overlay(m, wlog);
    // main menu: this mod's version under the game's version number, drawn by the panel (menuver.c, fgpanel.c)
    install_menuver(m, wlog);
    // other mods' DLLs from <game>\plugins (plugins.c), after every fix is in
    if (!INI(L"mod", L"plugins")) wlog("plugins off (plugins=0 in rome2fix.ini)");
    // from the load notification (another program loaded this DLL first) Windows refuses LoadLibrary (error 1168);
    // later, on a thread, the game would already be running. shortcut: not loaded then (Attila: 0 of 5 player logs)
    else if (g_late_install) {
        wchar_t pd[MAX_PATH];
        if (swprintf(pd, MAX_PATH, L"%ls\\plugins", dir) > 0 && GetFileAttributesW(pd) != INVALID_FILE_ATTRIBUTES)
            wlog("plugins NOT loaded: another program loaded dinput8.dll before the game, and Windows allows no DLL loading "
                 "at that point - this game start runs without the plugins folder");
    } else load_plugins(dir, wlog);
}

// Something else in the process (an overlay, another hook DLL) can load dinput8.dll before the game loads
// empire.retail.dll; Windows then never runs this DllMain again. So wait for the game DLL and install when it is
// mapped, which is still before its own startup code runs (the same state as the normal path).
typedef struct { USHORT Length, MaximumLength; PWSTR Buffer; } NOTIF_STR;
typedef struct { ULONG Flags; const NOTIF_STR *FullDllName, *BaseDllName; PVOID DllBase; ULONG SizeOfImage; } NOTIF_DATA;
static PVOID g_notif_cookie;

static VOID CALLBACK on_dll_loaded(ULONG reason, const NOTIF_DATA *d, PVOID ctx) {
    (void)ctx;
    static const wchar_t want[] = L"empire.retail.dll";
    if (reason != 1 || !d || !d->BaseDllName || d->BaseDllName->Length != sizeof want - sizeof(wchar_t)) return;
    if (_wcsnicmp(d->BaseDllName->Buffer, want, d->BaseDllName->Length / sizeof(wchar_t)) != 0) return;
    wlog("empire.retail.dll is loading now - installing the fixes");
    g_late_install = 1;
    install_all((HMODULE)d->DllBase);
}

static int wait_for_game(void) {
    typedef LONG(NTAPI * reg_fn)(ULONG, VOID(CALLBACK *)(ULONG, const NOTIF_DATA *, PVOID), PVOID, PVOID *);
    reg_fn reg = (reg_fn)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification");
    return reg && reg(0, on_dll_loaded, NULL, &g_notif_cookie) == 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) {
    (void)r;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    // One engine fix per game (Attila 7.4): a second copy in the same process (another dinput8.dll, one in plugins\, one
    // bundled under another name) would patch the game's code twice. The first copy holds Local\Rome2EngineFix_<pid> for
    // the process's life (other mods may open it to see that the fix runs); a later copy finds it and stays out. Copies
    // older than this one do not take it, so only they can still double up.
    wchar_t mx[64];
    _snwprintf(mx, 64, L"Local\\Rome2EngineFix_%lu", GetCurrentProcessId());
    HANDLE mh = CreateMutexW(NULL, FALSE, mx);
    int second = mh && GetLastError() == ERROR_ALREADY_EXISTS;
#ifndef PROFILING
    DisableThreadLibraryCalls(h);  // profiling builds decide in install_all (the heap test needs thread notifications)
#else
    if (second) DisableThreadLibraryCalls(h);   // its heap test never starts
#endif
    g_self = h;
    DWORD n = GetModuleFileNameW(h, g_log_path, MAX_PATH);
    wchar_t *sl = wcsrchr(g_log_path, L'\\');
    // room after the folder for the longest name built in it ("FrameBridge\settings.txt"); else no log path: do nothing
    if (n == 0 || n >= MAX_PATH || !sl || sl - g_log_path > MAX_PATH - 40) { if (second) CloseHandle(mh); return TRUE; }
    wcscpy(sl + 1, L"rome2fix.log");
    if (second) {
        CloseHandle(mh);
        wchar_t me[MAX_PATH] = L"";
        GetModuleFileNameW(h, me, MAX_PATH);
        wlog("Rome 2 Engine Performance Fix " MOD_VERSION_STR ": another copy is already running in this game (%ls) - this one "
             "(%ls) stays out; only one engine fix should be installed", mx, me);
        return TRUE;   // the dinput8 exports still forward to Windows' own DLL
    }
    log_header();
    HMODULE m = GetModuleHandleW(L"empire.retail.dll");
    if (m)
        install_all(m);
    else if (wait_for_game())
        wlog("dinput8.dll was loaded before empire.retail.dll (by another program or DLL) - the fixes install when "
             "the game loads it");
    else
        diag_startup(NULL, g_log_path, wlog);
    return TRUE;
}
