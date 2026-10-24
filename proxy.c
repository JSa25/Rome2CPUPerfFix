// dinput8.dll proxy for Total War: ROME II (empire.retail.dll of 2026-09-17).
// empire.retail.dll imports DINPUT8.dll, so this loads before the game's own startup code runs.
// It installs the fixes that profiling showed matter (see AGENT_LOG.md) and forwards the real
// DirectInput exports to the system dinput8.dll. Log: rome2fix.log next to this DLL.
// Built with -DPROFILING it also has the measuring tools (probes.txt, fps_log.txt, tilescan_ab.txt,
// sortfix_ab.txt); the release build has only the fixes, the off switches, diagnostics and the self-update.
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

int install_tilescan(HMODULE m, int mode, int fpslog, void (*log)(const char *fmt, ...));  // tilescan.c
extern void (*g_frame_check)(void);                                                    // tilescan.c
int install_x87fix(HMODULE m, void (*log)(const char *fmt, ...));                      // x87fix.c (from the Attila mod)
void x87fix_frame(void);                                                               // x87fix.c
int install_sortfix(HMODULE m, void (*log)(const char *fmt, ...));                     // sortfix.c (from the Attila mod)
void diag_startup(HMODULE empire, const wchar_t *log_path, void (*log)(const char *fmt, ...));  // diag.c
void update_check(HMODULE self, void (*log)(const char *fmt, ...));                             // update.c
#ifdef PROFILING
void sortfix_enable(int on);                                                           // sortfix.c
int install_probes(HMODULE m, const wchar_t *list, const wchar_t *out, void (*log)(const char *fmt, ...));  // probes.c
void probes_frame(void);                                                               // probes.c
extern void (*g_ab_flip)(int on);                                                      // tilescan.c
extern const char *g_ab_name;
static int g_probes_on;
#endif

static int g_x87_on;
static HMODULE g_self;

// Per frame on the main thread (from the tile-scan entry hook).
static void frame_checks(void) {
    if (g_x87_on) x87fix_frame();
#ifdef PROFILING
    if (g_probes_on) probes_frame();
#endif
}

static wchar_t g_log_path[MAX_PATH];

static void wlog(const char *fmt, ...) {
    FILE *f = _wfopen(g_log_path, L"a");
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a;
    va_start(a, fmt);
    vfprintf(f, fmt, a);
    va_end(a);
    fputc('\n', f);
    fclose(f);
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

HRESULT WINAPI stub_DirectInput8Create(HINSTANCE h, DWORD v, REFIID r, LPVOID *o, LPUNKNOWN u) {
    typedef HRESULT(WINAPI * fn)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
    fn f = (fn)real_proc("DirectInput8Create");
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

// Installs everything into the game DLL m (mapped and relocated, its own startup code not run yet).
static void install_all(HMODULE m) {
    diag_startup(m, g_log_path, wlog);  // version, process, game build, system, settings; crash records
    update_check(g_self, wlog);         // a newer Workshop copy replaces this DLL for the next launch
    // Off switches: empty files in the game folder.
    wchar_t f[MAX_PATH];
    wchar_t *name = wcsrchr(g_log_path, L'\\') - g_log_path + f + 1;
    wcscpy(f, g_log_path);
#define PRESENT(n) (wcscpy(name, n), GetFileAttributesW(f) != INVALID_FILE_ATTRIBUTES)
    int tile_mode = PRESENT(L"tilescan_off.txt") ? 0 : 1, fpslog = 0;
#ifdef PROFILING
    if (tile_mode && PRESENT(L"tilescan_ab.txt")) tile_mode = 2;
    fpslog = PRESENT(L"fps_log.txt");
#endif
    // The entry hook of the tile scan is also the per-frame callback the x87 fix needs for its safety check.
    if (!install_tilescan(m, tile_mode, fpslog, wlog))
        wlog("x87 fix not installed (needs the frame hook)");
    else if (PRESENT(L"x87fix_off.txt"))
        wlog("x87fix_off.txt found - x87 rounding fix not installed");
    else
        g_x87_on = install_x87fix(m, wlog) > 0;
    g_frame_check = frame_checks;
    if (PRESENT(L"sortfix_off.txt"))
        wlog("sortfix_off.txt found - render-list sorts not replaced");
#ifdef PROFILING
    else if (install_sortfix(m, wlog) == 2 && PRESENT(L"sortfix_ab.txt")) {
        // A/B: the sorts flip on/off every 10 s at frame start, fps logged per window.
        g_ab_name = "sortfix";
        g_ab_flip = sortfix_enable;
        wlog("sortfix_ab.txt found - A/B mode for the sort fix");
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
#else
    else
        install_sortfix(m, wlog);
#endif
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
    DisableThreadLibraryCalls(h);
    g_self = h;
    DWORD n = GetModuleFileNameW(h, g_log_path, MAX_PATH);
    wchar_t *sl = wcsrchr(g_log_path, L'\\');
    if (n == 0 || n >= MAX_PATH || !sl || sl - g_log_path > MAX_PATH - 20) return TRUE;  // no log path: do nothing
    wcscpy(sl + 1, L"rome2fix.log");
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
