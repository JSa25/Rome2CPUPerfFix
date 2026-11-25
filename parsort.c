// The soldiers' draw lists sorted on several threads (from the Attila mod's sortfix.c, install_parsort: Attila AGENT_LOG
// 238, PERF_DEEP_DIVE.md B1, +11 % fps measured there in big melees; nothing measured in Rome 2 yet).
// Rome 2 is shaped differently from Attila: there is no list job. The main thread itself runs FUN_11503a00 (Steam; a
// virtual function, .rdata +0x19347F0), which walks eight mesh nodes one after the other, each inline: the node's mesh
// sort (+0x14A7E50), then 15 passes x every mesh of the node: mesh vtable +0x138(pass) merges that pass's list, sorts it
// and appends it to the owner's master list. For the soldiers' meshes +0x138 is +0x1543690: list = mesh + 0x244 +
// pass * 0x54 (count +8, array +0xC), merge +0x147EFE0, the draw-list sort +0x14A8370 (sortfix.c's radix sort), append
// to [mesh + 0x214] + 0x4C. Battle capture (captures/battle, 22,672 main-thread samples, before sortfix): FUN_11503a00
// 9.5 % of the main thread, the soldiers' +0x1543690 5.0 %, the sort in it 3.0 %; every +0x1543690 sample came through
// the first node's loop (return +0x1503B71); no other thread had a sample in either.
// Here the first node's mesh sort call goes through ps_mesh_sort: after the game's mesh sort, if every mesh of the node is
// a soldier mesh, every pass list is merged here (the game's merge, on the main thread), and the lists are sorted by this
// thread and up to three helper threads of ours, with sortfix.c's draw_sort. Then the game's loop runs unchanged: its
// merges find nothing to do, and its sort calls (the soldier function's call, retargeted to ps_list_sort) return at once
// for the lists sorted here, in the order they were sorted here; any other call is sorted as before. A list's content
// after merge + sort depends on that list alone, so the result is the same. The helpers run nothing but draw_sort.
#include <windows.h>
#include <string.h>
#include "builds.h"

void __cdecl draw_sort(void **first, void **last, int ideal, void *ctx);   // sortfix.c: what the game's draw-list sort jumps to

#define LIST_PASSES 15        // cmp eax,0Fh after the node's loop
#define LIST_FIRST 0x244
#define LIST_SIZE 0x54
#define VT_LISTS 0x138        // the mesh's vtable slot: merge, sort and append pass p's list
#define LEN_BLOCK 0x10D       // the first node's block: lea edi,[ebx+0ACh] .. the call after its loop
#define LEN_SOLDIER 0x9D
#define LEN_MERGE 0x118
#define AT_MESH_SORT 0x48     // in the block: call mesh sort
#define AT_BLOCK_END 0x108    // in the block: the call after the loop (thiscall, 4 arguments)
#define AT_LIST_SORT 0x3D     // in the soldier function: call the draw-list sort
// per known build (builds.h order): the block, the soldier function (vtable +0x138), the merge, each with FNV-1a of its
// code with relocations undone. The three builds' code is the same instruction for instruction (capstone, call targets
// aside); the functions were found by tools/match_attila.py from Steam (265/265, 17/17, 32/32 windows).
static const DWORD g_ps_builds[NBUILDS][6] = {
    {0x1503ABE, 0xB72C6890u, 0x1543690, 0x5835249Au, 0x147EFE0, 0xE6561081u},   // Steam 69CE2041
    {0x14DD62E, 0x3B057E94u, 0x151D150, 0xCEF58957u, 0x1458800, 0x02974D31u},   // pantheon beta 6AB978BD
    {0x15038AE, 0x2F39BF50u, 0x1543460, 0xFE0666C8u, 0x147EDD0, 0xD9D082BDu},   // GOG 6A564E92
};

typedef void(__cdecl *sort_fn)(void **, void **, int, void *);
struct sort_task { void **first; unsigned n; };
static struct sort_task *volatile g_tasks;   // the tasks being handed out (the owner thread's t_tasks)
static __thread struct sort_task *t_tasks;   // this thread's last node: its lists, in the order the game's loop sorts them
static __thread unsigned t_tasks_cap, t_count, t_next;   // t_next..t_count: lists sorted here, not yet met in the loop
static volatile LONG g_task_next, g_task_count, g_task_pending, g_task_inloop, g_par_busy;
static HANDLE g_par_sem;
static int g_par_threads;                     // helper threads running
int g_parsort_helpers = -1;                   // helpers to use: -1 = by the CPU (set at install); the test sets it
unsigned g_parsort_min = 8192;                // fewer items than this in a frame: not worth waking anyone (Attila's figure)
volatile unsigned char g_parsort_on = 1;      // off: the game's loop does it all (the A/B below, and the test)
static int g_parsort_ab;                      // parsort_ab.txt (FPS test): on in even 10 s windows of UTC time, off in odd
volatile LONG g_parsort_jobs;                 // nodes whose lists were sorted here (the test reads it)
static void *g_soldier;                       // the soldier meshes' vtable +0x138
static void (__thiscall *g_list_merge)(void *list);
static sort_fn g_mesh_sort, g_list_sort;      // the calls' own targets: the mesh sort, the draw-list sort's entry
static void (__thiscall *g_block_end)(void *self, DWORD a, DWORD b, DWORD c, DWORD d);
static void (*g_par_log)(const char *fmt, ...);
static __thread LARGE_INTEGER t_block_t0;
static __thread int t_block_on, t_block_par, t_block_soldiers;

static void *vt_lists(void *mesh) { return (*(void ***)mesh)[VT_LISTS / 4]; }

// The game's draw-list sort jumps to sortfix.c's draw_sort (its A/B switch can put the game's own sort back, whose order
// among equal keys is not draw_sort's: then the game's loop sorts, as before).
static int draw_sort_is_ours(void) {
    const unsigned char *e = (const unsigned char *)(void *)g_list_sort;
    return e[0] == 0xE9 && e + 5 + *(const LONG *)(e + 1) == (const unsigned char *)(void *)draw_sort;
}

// Takes tasks until none is left. A task is taken with a compare-and-swap on the next index, so a thread that read an
// old index can only take a task that is still free; the owner waits for g_task_inloop == 0 before it hands out the
// next node's tasks, so no thread checks an index against one node's count and takes it in another.
static void run_sort_tasks(void) {
    for (;;) {
        LONG i = g_task_next;
        if (i >= g_task_count) return;
        if (InterlockedCompareExchange(&g_task_next, i + 1, i) != i) continue;
        struct sort_task *t = &g_tasks[i];
        draw_sort(t->first, t->first + t->n, (int)t->n, NULL);
        InterlockedDecrement(&g_task_pending);
    }
}

static DWORD WINAPI sort_helper(void *x) {
    (void)x;
    for (;;) {
        WaitForSingleObject(g_par_sem, INFINITE);
        InterlockedIncrement(&g_task_inloop);
        run_sort_tasks();
        InterlockedDecrement(&g_task_inloop);
    }
}

// 1: every list of this node is merged and sorted (t_tasks[0..t_count) in the loop's order). 0: nothing was sorted here
// (other mesh classes, a small frame, no helpers, the game's own sort in place, or another node is using the helpers)
// and the game's loop does it all, as before.
static int sort_lists_parallel(void **mesh, unsigned meshes) {
    if (!meshes || vt_lists(mesh[0]) != g_soldier) return 0;
    for (unsigned m = 1; m < meshes; m++)
        if (vt_lists(mesh[m]) != g_soldier) return 0;   // a node of mixed mesh classes: left to the game
    if (g_parsort_helpers <= 0 || !draw_sort_is_ours() || InterlockedCompareExchange(&g_par_busy, 1, 0)) return 0;
    int done = 0;
    unsigned need = meshes * LIST_PASSES, n = 0, items = 0;
    if (need > t_tasks_cap) {
        void *t = t_tasks ? HeapReAlloc(GetProcessHeap(), 0, t_tasks, need * 2 * sizeof *t_tasks) : HeapAlloc(GetProcessHeap(), 0, need * 2 * sizeof *t_tasks);
        if (!t) goto out;
        t_tasks = t;
        t_tasks_cap = need * 2;
    }
    for (unsigned p = 0; p < LIST_PASSES; p++)   // the game's loop order: pass by pass, mesh by mesh
        for (unsigned m = 0; m < meshes; m++) {
            unsigned char *list = (unsigned char *)mesh[m] + LIST_FIRST + p * LIST_SIZE;
            g_list_merge(list);
            unsigned c = *(unsigned *)(list + 8);
            if (c < 2) continue;
            t_tasks[n].first = *(void ***)(list + 0xC);
            t_tasks[n++].n = c;
            items += c;
        }
    if (n < 2 || items < g_parsort_min) goto out;
    if (!g_par_sem) g_par_sem = CreateSemaphoreW(NULL, 0, 64, NULL);
    while (g_par_sem && g_par_threads < g_parsort_helpers) {
        // 256 KB of address space each, not the exe's default stack: the 32-bit game has none to spare
        HANDLE h = CreateThread(NULL, 0x40000, sort_helper, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        if (!h) break;
        // The main thread waits for a helper that holds a list: one step above normal, so a helper is not parked in
        // the middle of a list when every core is busy.
        SetThreadPriority(h, THREAD_PRIORITY_ABOVE_NORMAL);
        CloseHandle(h);
        g_par_threads++;
    }
    while (g_task_inloop) YieldProcessor();   // a helper that woke late is still looking at the last node's counters
    g_tasks = t_tasks;
    g_task_next = 0;
    g_task_pending = (LONG)n;
    InterlockedExchange(&g_task_count, (LONG)n);   // open: tasks can be taken from here on
    int wake = g_par_threads < g_parsort_helpers ? g_par_threads : g_parsort_helpers;
    if (wake > 0) ReleaseSemaphore(g_par_sem, wake, NULL);
    run_sort_tasks();
    for (unsigned spin = 0; g_task_pending; spin++) {   // a helper is finishing the list it took
        if (spin < 4000) YieldProcessor(); else SwitchToThread();
    }
    InterlockedExchange(&g_task_count, 0);         // closed
    InterlockedIncrement(&g_parsort_jobs);
    t_count = n;
    done = 1;
out:
    InterlockedExchange(&g_par_busy, 0);
    return done;
}

// The first node's mesh sort (cdecl, the game's arguments): the node's meshes are first..last.
void __cdecl ps_mesh_sort(void **first, void **last, int ideal, void *ctx) {
    g_mesh_sort(first, last, ideal, ctx);
    t_count = t_next = 0;
    if (g_parsort_ab) {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);   // 100 ns units
        g_parsort_on = !((((ULONGLONG)ft.dwHighDateTime << 32 | ft.dwLowDateTime) / 100000000ULL) & 1);
        t_block_soldiers = last > first && vt_lists(first[0]) == g_soldier;
        QueryPerformanceCounter(&t_block_t0);   // the block's time: this mesh sort .. the call after its loop
    }
    t_block_on = g_parsort_on;
    t_block_par = t_block_on && sort_lists_parallel(first, (unsigned)(last - first));
}

// The soldier function's sort call: a list sorted above, met in the same order, is not sorted again.
static void __cdecl ps_list_sort(void **first, void **last, int ideal, void *ctx) {
    if (t_next < t_count && t_tasks[t_next].first == first && t_tasks[t_next].n == (unsigned)(last - first)) {
        t_next++;
        return;
    }
    g_list_sort(first, last, ideal, ctx);
}

// With parsort_ab.txt: how long the soldiers' block takes in the on windows and in the off windows, to the log every
// 4096 blocks. `on`: the window; `par`: this block's lists were sorted here (a small frame in an on window is not).
static void block_stats(int on, int par, LONGLONG ticks) {
    static LONGLONG t[2], worst[2], f;
    static unsigned jobs[2], sorted;
    t[on] += ticks;
    sorted += par;
    if (ticks > worst[on]) worst[on] = ticks;   // a helper that held the block up shows here, not in the average
    if (++jobs[on], (jobs[0] + jobs[1]) % 4096) return;
    if (!f) { LARGE_INTEGER q; QueryPerformanceFrequency(&q); f = q.QuadPart; }
    g_par_log("soldier draw lists A/B: ON windows %u frames (%u with the lists sorted on %d threads), %.0f us each (worst %.0f); "
              "off windows %u frames on one thread, %.0f us each (worst %.0f)",
              jobs[1], sorted, g_par_threads + 1, jobs[1] ? t[1] * 1e6 / f / jobs[1] : 0.0, worst[1] * 1e6 / f,
              jobs[0], jobs[0] ? t[0] * 1e6 / f / jobs[0] : 0.0, worst[0] * 1e6 / f);
    t[0] = t[1] = worst[0] = worst[1] = 0;
    jobs[0] = jobs[1] = sorted = 0;
}

// The call after the first node's loop (installed with parsort_ab.txt only): the block's time, then the game's call.
void __thiscall ps_block_end(void *self, DWORD a, DWORD b, DWORD c, DWORD d) {
    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    t_count = t_next = 0;
    if (g_parsort_ab && t_block_soldiers) block_stats(t_block_on, t_block_par, t1.QuadPart - t_block_t0.QuadPart);
    g_block_end(self, a, b, c, d);
}

// a call instruction's target, retargeted to `to` (the call stays a call)
static void *retarget(unsigned char *site, void *to) {
    void *was = site + 5 + *(LONG *)(site + 1);
    DWORD old;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) return NULL;
    LONG rel = (LONG)((unsigned char *)to - (site + 5));
    memcpy(site + 1, &rel, 4);
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    return was;
}

// 1: the first node's lists are sorted on several threads. Needs sortfix.c's draw-list sort (install_sortfix first).
// ab: parsort_ab.txt is in the game folder.
int install_parsort(HMODULE m, int ab, void (*log)(const char *fmt, ...)) {
    unsigned char *b = (unsigned char *)m;
    const DWORD *e = NULL;
    for (int i = 0; i < NBUILDS && !e; i++) {
        const DWORD *x = g_ps_builds[i];
        if (fnv1a_unreloc(b, x[0], LEN_BLOCK) == x[1] && fnv1a_unreloc(b, x[2], LEN_SOLDIER) == x[3] && fnv1a_unreloc(b, x[4], LEN_MERGE) == x[5])
            e = x;
    }
    if (!e) { log("soldier draw lists on several threads: the game's list code differs from the known game versions - not installed"); return 0; }
    unsigned char *blk = b + e[0], *sol = b + e[2];
    g_list_sort = (sort_fn)(void *)(sol + AT_LIST_SORT + 5 + *(LONG *)(sol + AT_LIST_SORT + 1));
    if (!draw_sort_is_ours()) { log("soldier draw lists on several threads: not installed (it needs the draw-list sort)"); return 0; }
    if (g_parsort_helpers < 0) {   // the game's own threads keep theirs: one helper from 8 logical processors, three from 16
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        g_parsort_helpers = si.dwNumberOfProcessors >= 16 ? 3 : si.dwNumberOfProcessors >= 12 ? 2 : si.dwNumberOfProcessors >= 8;
    }
    if (!g_parsort_helpers) { log("soldier draw lists on several threads: left out (fewer than 8 logical processors)"); return 0; }
    g_soldier = sol;
    g_list_merge = (void(__thiscall *)(void *))(void *)(b + e[4]);
    g_par_log = log;
    g_parsort_ab = ab;
    // the sort call first: until the mesh sort's call is ours, nothing is sorted here and every call goes through
    if (!retarget(sol + AT_LIST_SORT, (void *)ps_list_sort)) goto fail;
    if (ab && !(g_block_end = (void(__thiscall *)(void *, DWORD, DWORD, DWORD, DWORD))retarget(blk + AT_BLOCK_END, (void *)ps_block_end))) goto fail;
    if (!(g_mesh_sort = (sort_fn)retarget(blk + AT_MESH_SORT, (void *)ps_mesh_sort))) goto fail;
    log("soldier draw lists sorted on %d threads (the main thread + %d helpers)", g_parsort_helpers + 1, g_parsort_helpers);
    if (ab) log("soldier draw lists A/B: on in even 10 s windows of UTC time, off in odd (parsort_ab.txt)");
    return 1;
fail:   // shortcut: a call already retargeted stays so; each forwards to the game's own target and alone changes nothing
    log("soldier draw lists on several threads: VirtualProtect failed (%lu) - not installed", GetLastError());
    return 0;
}
