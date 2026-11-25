// Test for diag.c: startup lines for a non-game process and for a "game" (this exe stands in for empire.retail.dll),
// then 10 deliberate access violations (recovered by a second handler): the log must get 8 EXCEPTION records with
// the right code, access type, address and a stack scan, and the program must keep running.
// usage: diag_test.exe   (writes diag_test.log next to the exe)
#include <stdarg.h>
#include "../diag.c"

volatile LONG g_frame_no;   // stands in for fpslog.c's frame counter: the watchdog runs

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static wchar_t g_path[MAX_PATH];
static void say(const char *fmt, ...) {
    FILE *f = _wfopen(g_path, L"a");
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a); fputc('\n', f); fclose(f);
}

extern char fault_end[];
static void WINAPI overflow_done(void) { ExitThread(0); }
static LONG CALLBACK recover(EXCEPTION_POINTERS *e) {
    if (e->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW) {   // end the overflowing thread from fresh stack
        e->ContextRecord->Esp = (DWORD)(UINT_PTR)((NT_TIB *)NtCurrentTeb())->StackBase - 256;
        e->ContextRecord->Eip = (DWORD)(UINT_PTR)overflow_done;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    e->ContextRecord->Eip = (DWORD)(UINT_PTR)fault_end;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static __attribute__((noinline)) void fault(void) {
    __asm__ volatile("movl $1, 0x00000000\n\t.globl _fault_end\n_fault_end:\n\t" ::: "memory");
}

static __attribute__((noinline)) int deep(volatile int n) {
    volatile char pad[200];
    pad[0] = (char)n;
    return deep(n + 1) + pad[0];
}
static DWORD WINAPI overflow_thread(void *x) { (void)x; return (DWORD)deep(0); }

// image_of on a module whose header cannot be read or is garbage (Attila review 3 #6): a real DLL mapped as an image,
// its header page made no-access, then readable with e_lfanew garbage; the old code faulted inside the crash logger
static void bad_headers(void) {
    wchar_t p[MAX_PATH];
    GetSystemDirectoryW(p, MAX_PATH);   // SysWOW64 for this 32-bit test
    wcscat(p, L"\\winmm.dll");
    HANDLE f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(f != INVALID_HANDLE_VALUE);
    HANDLE s = CreateFileMappingW(f, NULL, PAGE_READONLY | SEC_IMAGE, 0, 0, NULL);
    CHECK(s);
    BYTE *b = MapViewOfFile(s, FILE_MAP_READ, 0, 0, 0);
    CHECK(b);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    DWORD code = (DWORD)(UINT_PTR)b + nt->OptionalHeader.BaseOfCode + 16, base = 0, old;
    CHECK(image_of(code, &base) && base == (DWORD)(UINT_PTR)b);   // a sound header: its export name
    CHECK(VirtualProtect(b, 4096, PAGE_NOACCESS, &old));
    const char *n1 = image_of(code, &base);
    CHECK(n1 && !strcmp(n1, "image"));
    CHECK(VirtualProtect(b, 4096, PAGE_WRITECOPY, &old));
    ((IMAGE_DOS_HEADER *)b)->e_lfanew = 0x7FFFFFF0;
    const char *n2 = image_of(code, &base);
    CHECK(n2 && !strcmp(n2, "image"));
    UnmapViewOfFile(b); CloseHandle(s); CloseHandle(f);
    printf("module header checks passed (no-access header, garbage e_lfanew)\n");
}

// A fault whose ESP is wild (committed but no-access, then a guard page, both outside this thread's stack): the stack
// scan must not read there. The old scan faulted inside the handler (Attila audit 2026-11-20): the record was lost and
// this process dies here. Run as "diag_test.exe wildesp".
static int wild_esp(void) {
    wcscpy(wcsrchr(g_path, L'\\') + 1, L"diag_wildesp.log");
    DeleteFileW(g_path);
    diag_startup(GetModuleHandleW(NULL), g_path, say);
    g_dumped = 1;   // no minidump of a made-up context
    BYTE *na = VirtualAlloc(NULL, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    BYTE *gd = VirtualAlloc(NULL, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE | PAGE_GUARD);
    CHECK(na && gd);
    BYTE *at[2] = {na + 256, gd + 256};
    for (int i = 0; i < 2; i++) {
        EXCEPTION_RECORD er = {EXCEPTION_ACCESS_VIOLATION, 0, NULL, (void *)fault, 2, {0, 0}};
        CONTEXT cx = {0};
        cx.Esp = (DWORD)(UINT_PTR)at[i];
        EXCEPTION_POINTERS ep = {&er, &cx};
        record(&ep, 0);
    }
    static char text[65536];
    FILE *f = _wfopen(g_path, L"rb");
    size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = 0;
    char *r1 = strstr(text, "] EXCEPTION 1:"), *r2 = strstr(text, "] EXCEPTION 2:");
    CHECK(r1 && r2 && strstr(r1, "  stack:\n") && strstr(r2, "  stack:\n"));
    DeleteFileW(g_path);
    printf("wild ESP (no-access, guard page): both records written, no stack read\n");
    return 0;
}

int main(int argc, char **argv) {
    GetModuleFileNameW(NULL, g_path, MAX_PATH);
    if (argc > 1 && !strcmp(argv[1], "wildesp")) return wild_esp();
    if (wcsstr(GetCommandLineW(), L"diag_mods.txt;") || wcsstr(GetCommandLineW(), L"diag mods.txt;")) {   // the children below: started like the launcher / a mod manager start the game
        wcscpy(wcsrchr(g_path, L'\\') + 1, L"diag_mods.log");
        diag_startup(GetModuleHandleW(NULL), g_path, say);
        return 0;
    }
    {   // the launcher's mod list: named on the command line, in the game folder, UTF-16 with a BOM
        wchar_t mp[MAX_PATH], exe[MAX_PATH], cmd[MAX_PATH + 32];
        wcscpy(exe, g_path);
        wcscpy(mp, g_path);
        wcscpy(wcsrchr(mp, L'\\') + 1, L"diag_mods.txt");
        FILE *m = _wfopen(mp, L"wb");
        static const wchar_t body[] = L"\xFEFFmod \"a_first.pack\";\r\nmod \"b_second.pack\";\r\n";
        fwrite(body, sizeof(wchar_t), wcslen(body), m);
        fclose(m);
        wcscpy(wcsrchr(g_path, L'\\') + 1, L"diag_mods.log");
        DeleteFileW(g_path);
        swprintf(cmd, MAX_PATH + 32, L"\"%ls\"  diag_mods.txt;", exe);
        STARTUPINFOW si = {sizeof si};
        PROCESS_INFORMATION pi;
        CHECK(CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi));
        WaitForSingleObject(pi.hProcess, 30000);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        static char t[65536];
        FILE *lf = _wfopen(g_path, L"rb");
        size_t tn = lf ? fread(t, 1, sizeof t - 1, lf) : 0;
        if (lf) fclose(lf);
        t[tn] = 0;
        CHECK(strstr(t, "mods on: 2 - a_first.pack, b_second.pack"));
        DeleteFileW(mp);
        DeleteFileW(g_path);
        // a mod manager's form: the full path quoted, a space in it, 100 mods (ASCII): all counted, the list cut with "..."
        wchar_t mq[MAX_PATH];
        wcscpy(mq, exe);
        wcscpy(wcsrchr(mq, L'\\') + 1, L"diag mods.txt");
        m = _wfopen(mq, L"wb");
        for (int i = 0; i < 100; i++) fprintf(m, "mod \"mod_number_%03d.pack\";\r\n", i);
        fclose(m);
        swprintf(cmd, MAX_PATH + 32, L"\"%ls\" \"%ls;\"", exe, mq);
        CHECK(CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi));
        WaitForSingleObject(pi.hProcess, 30000);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        lf = _wfopen(g_path, L"rb");
        tn = lf ? fread(t, 1, sizeof t - 1, lf) : 0;
        if (lf) fclose(lf);
        t[tn] = 0;
        CHECK(strstr(t, "mods on: 100 - mod_number_000.pack, ") && strstr(t, ", ..."));
        DeleteFileW(mq);
        DeleteFileW(g_path);
        wcscpy(g_path, exe);
    }
    wcscpy(wcsrchr(g_path, L'\\') + 1, L"diag_test.log");
    DeleteFileW(g_path);
    diag_startup(NULL, g_path, say);                     // not the game: one explanation, no handler
    diag_startup(GetModuleHandleW(NULL), g_path, say);   // "the game": full startup lines + handler
    wchar_t dir[MAX_PATH];
    wcscpy(dir, g_path);
    *wcsrchr(dir, L'\\') = 0;
    bad_headers();
    AddVectoredExceptionHandler(0, recover);             // after diag's, like the game's own handling
    HANDLE th = CreateThread(NULL, 0, overflow_thread, NULL, 0, NULL);   // record 1: a runaway recursion
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
    for (int i = 0; i < 10; i++) {
        g_uikeep_tid = i == 3 ? GetCurrentThreadId() : 0;  // one crash during the menu texture fix's work
        fault();
    }
    g_uikeep_tid = 0;
    static char text[65536];
    FILE *f = _wfopen(g_path, L"rb");
    size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = 0;
    fputs(text, stdout);
    int recs = 0;
    for (char *s = text; (s = strstr(s, "] EXCEPTION ")) != NULL; s++) recs++;
    CHECK(strstr(text, "nothing patched in this process"));
    CHECK(strstr(text, "system: Windows ") && strstr(text, "logical CPUs"));
    CHECK(strstr(text, "game: empire.retail.dll build"));
    CHECK(strstr(text, "other hook DLLs in the game folder:"));
    CHECK(strstr(text, "GPU: ") && strstr(text, "memory: RAM "));
    CHECK(recs == MAX_RECORDS);
    CHECK(strstr(text, "0xC0000005 access violation at exe+0x"));
    CHECK(strstr(text, "write to address 00000000"));
    CHECK(strstr(text, "main thread"));
    CHECK(strstr(text, "  stack: "));
    char *so = strstr(text, "0xC00000FD stack overflow at exe+0x");   // its stack scan shows the recursion
    CHECK(so && strstr(so, "  stack: exe+0x") && strstr(so, "  stack: exe+0x") < strstr(so, "] EXCEPTION 2:"));
    char *ol = strstr(so, "  outside the loop (from ");   // and the frames that started it: the thread's own start
    CHECK(strstr(so, "module-pointing values: exe ") && ol && ol < strstr(so, "] EXCEPTION 2:") && strstr(ol, "exe+0x"));
    char *u = strstr(text, "create an empty uikeep_off.txt");  // only in the 4th record
    CHECK(u && !strstr(u + 1, "create an empty uikeep_off.txt"));
    CHECK(strstr(text, "] EXCEPTION 5:") < u && (!strstr(text, "] EXCEPTION 6:") || u < strstr(text, "] EXCEPTION 6:")));
    // the first record (the stack overflow, on a thread with no stack left): modules, uptime, and a minidump from the
    // dump thread; once per session
    CHECK(strstr(so, "  game running ") && strstr(so, "  game running ") < strstr(so, "  stack: "));
    char *mo = strstr(text, "  modules not from Windows: ");
    CHECK(mo && mo < strstr(text, "] EXCEPTION 2:") && strstr(mo, "diag_test.exe") && !strstr(mo + 10, "modules not from Windows"));
    char *dl = strstr(text, "  crash dump: crash_");
    CHECK(dl && dl < strstr(text, "] EXCEPTION 2:") && strstr(dl, ".dmp (") && !strstr(dl + 10, "crash dump:"));
    wchar_t dp[MAX_PATH];
    swprintf(dp, MAX_PATH, L"%ls\\%.25hs", dir, dl + 14);   // crash_YYYYMMDD_HHMMSS.dmp
    WIN32_FILE_ATTRIBUTE_DATA fa;
    CHECK(GetFileAttributesExW(dp, GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 4096);
    DeleteFileW(dp);
    CHECK(strstr(text, "game drive: ") && strstr(text, "GB free"));
    CHECK(strstr(text, "mods on: "));
    // freeze watchdog: frames run, then the main thread stops for 22 s, then a frame again
    g_frame_no = 5;
    Sleep(2500);
    Sleep(22000);
    g_frame_no++;
    Sleep(2500);
    f = _wfopen(g_path, L"rb");
    n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = 0;
    char *fz = strstr(text, "FREEZE? the main thread has finished no frame for 20 s");
    CHECK(fz && strstr(fz, "  at ") && strstr(fz, "  stack: ") && !strstr(fz + 10, "FREEZE?"));
    CHECK(strstr(fz, "[watchdog] main thread finished a frame again after 2"));
    printf("%s", fz);
    printf("\nall diag tests passed (%d records for 10 faults)\n", recs);
    return 0;
}
