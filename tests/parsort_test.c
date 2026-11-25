// The soldiers' node with its lists sorted on several threads (parsort.c) against the same node on one thread, on the
// game's own code: the game's one-node function (Steam +0x14ADAC0, the same loop as the first node's block inside
// +0x1503A00, which install_parsort patches and which cannot run here) with its mesh sort, the soldier meshes' list
// function (merge, sort, append), the merge and the push_back all run from the mapped DLL, on built scenes (a node,
// meshes, 15 lists per mesh in per-thread vectors, master lists). The test points that function's mesh sort call at
// ps_mesh_sort, as install_parsort does for the block. Every list, every master list and the node's totals must come
// out the same.
// usage: parsort_test.exe <path to empire.retail.dll>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "../sortfix.c"
#include "../parsort.c"

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static void say(const char *fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); putchar('\n'); }
static int g_said;
static void say_count(const char *fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); putchar('\n'); g_said++; }

// ---- memory: one arena per thread, emptied per scene. The game's allocator is pointed here, so a helper thread that
// allocated through the game (it must not) would find no arena and stop the test.
#define ARENA (192u << 20)
static __thread unsigned char *t_arena;
static __thread unsigned t_used;
static void *take(unsigned n) {
    CHECK(t_arena != NULL);
    n = (n + 15) & ~15u;
    CHECK(t_used + n <= ARENA);
    void *p = t_arena + t_used;
    t_used += n;
    memset(p, 0, n);
    return p;
}
static void *__cdecl game_new(unsigned size, int flag) { (void)flag; return take(size ? size : 1); }
static void __cdecl game_delete(void *p) { (void)p; }

static __thread unsigned t_rng;
static unsigned rnd(unsigned n) { t_rng = t_rng * 1103515245u + 12345u; return (t_rng >> 8) % n; }

// ---- a scene ----
#define THREADS 4
#define PASSES LIST_PASSES
#define OTHER_FIRST 0x214   // the other mesh class's lists (vtable +0x138 = soldier function - 0x170, the second sort, no append)
#define OWNER 0x214         // soldier mesh: its owner, master list at owner + 0x4C
enum { SOLDIERS, OTHER_CLASS, MIXED };
struct scene {
    unsigned char *node, **mesh, **owner;
    unsigned meshes, owners;
};
static void *g_vt_soldier[0x150 / 4], *g_vt_other[0x150 / 4];
static int __thiscall mesh_key(void *self) { return *(int *)((char *)self + 0x10); }   // vtable +0xE8: the mesh's sort key

// one of the engine's per-thread lists at `l`: `n` pointers spread over the threads' vectors (and sometimes a few
// already in the list's own vector), marked as needing a merge
static void fill_list(unsigned char *l, void **src, unsigned n) {
    unsigned char *tv = take(THREADS * 12);
    *(unsigned *)(l + 0x34) = THREADS;
    *(unsigned char **)(l + 0x38) = tv;
    *(unsigned **)(l + 0x44) = take((THREADS + 1) * 4);   // the merge writes each thread's offset here
    unsigned own = n > 4 && rnd(8) == 0 ? rnd(n / 2) : 0;   // already merged earlier
    if (own) {
        void **d = game_new(own * 4, 0);
        memcpy(d, src, own * 4);
        *(unsigned *)(l + 4) = own;
        *(unsigned *)(l + 8) = own;
        *(void ***)(l + 0xC) = d;
    }
    unsigned left = n - own, at = own;
    for (unsigned t = 0; t < THREADS; t++) {
        unsigned c = t == THREADS - 1 ? left : rnd(left + 1);
        void **d = take(c * 4 + 4);
        memcpy(d, src + at, c * 4);
        *(unsigned *)(tv + t * 12) = c;
        *(unsigned *)(tv + t * 12 + 4) = c;
        *(void ***)(tv + t * 12 + 8) = d;
        at += c;
        left -= c;
    }
    l[0x4C] = n != own || rnd(2);   // nothing to merge: sometimes not even marked
}

static void build(struct scene *s, unsigned seed, int kind, unsigned meshes, unsigned items, unsigned big) {
    t_used = 0;
    t_rng = seed * 2654435761u + 1;
    unsigned char *pool = take(items * 0x80);
    for (unsigned i = 0; i < items; i++) {
        unsigned char *it = pool + i * 0x80;
        *(unsigned *)it = i;
        *(unsigned *)(it + 0x68) = rnd(4) ? rnd(300) : rnd(0xFFFFFF);   // many equal keys: the order among them must hold too
        *(unsigned *)(it + 0x6C) = rnd(3);
        *(unsigned *)(it + 0x40) = rnd(50);
        *(unsigned *)(it + 0x44) = rnd(2);
    }
    s->meshes = meshes;
    s->owners = meshes / 3 + 1;
    s->mesh = take(meshes * 4 + 4);
    s->owner = take(s->owners * 4);
    for (unsigned i = 0; i < s->owners; i++) s->owner[i] = take(0x60);
    void **scratch = take((big + 300) * 4);
    for (unsigned i = 0; i < meshes; i++) {
        unsigned char *m = take(0x740);
        int soldier = kind == SOLDIERS || (kind == MIXED && i != meshes / 2);
        *(void ***)m = soldier ? g_vt_soldier : g_vt_other;
        *(unsigned *)(m + 0x14) = i;
        if (soldier) *(unsigned char **)(m + OWNER) = s->owner[rnd(s->owners)];
        for (unsigned p = 0; p < PASSES; p++) {
            unsigned r = rnd(100), n = r < 40 ? 0 : r < 50 ? 1 : r < 60 ? 2 + rnd(31) : r < 97 ? 33 + rnd(260) : big ? rnd(big) : 5;
            if (!items) n = 0;
            for (unsigned k = 0; k < n; k++) scratch[k] = pool + rnd(items) * 0x80;
            fill_list(m + (soldier ? LIST_FIRST : OTHER_FIRST) + p * LIST_SIZE, scratch, n);
        }
        s->mesh[i] = m;
    }
    for (unsigned i = meshes; i > 1; i--) {   // unique sort keys in a shuffled order
        unsigned j = rnd(i);
        unsigned char *t = s->mesh[i - 1]; s->mesh[i - 1] = s->mesh[j]; s->mesh[j] = t;
    }
    for (unsigned i = 0; i < meshes; i++) *(int *)(s->mesh[i] + 0x10) = (int)(i * 7 + 3);
    for (unsigned i = meshes; i > 1; i--) {
        unsigned j = rnd(i);
        unsigned char *t = s->mesh[i - 1]; s->mesh[i - 1] = s->mesh[j]; s->mesh[j] = t;
    }
    // the node, already merged (its merge takes a critical section through an import this mapping has not resolved)
    s->node = take(0x100);
    void **nm = game_new(meshes * 4 + 4, 0);
    memcpy(nm, s->mesh, meshes * 4);
    *(unsigned *)(s->node + 4) = meshes;
    *(unsigned *)(s->node + 8) = meshes;
    *(void ***)(s->node + 0xC) = nm;
    for (unsigned p = 0; p < PASSES; p++) *(unsigned *)(s->node + 0x58 + p * 4) = *(unsigned *)(s->node + 0x94 + p * 4) = 0xDDDDDDDD;
}

// everything the node function leaves behind, with items and meshes by their numbers (addresses differ from run to run)
static unsigned long long g_h;
static void mix(unsigned v) { g_h = (g_h ^ v) * 0x100000001B3ull; }
static unsigned long long result(const struct scene *s) {
    g_h = 0xCBF29CE484222325ull;
    unsigned nm = *(unsigned *)(s->node + 8);
    unsigned char **order = *(unsigned char ***)(s->node + 0xC);
    mix(nm);
    for (unsigned i = 0; i < nm; i++) mix(*(unsigned *)(order[i] + 0x14));
    for (unsigned p = 0; p < PASSES; p++) { mix(*(unsigned *)(s->node + 0x58 + p * 4)); mix(*(unsigned *)(s->node + 0x94 + p * 4)); }
    for (unsigned i = 0; i < s->meshes; i++) {
        unsigned char *m = s->mesh[i];
        unsigned at = *(void ***)m == g_vt_soldier ? LIST_FIRST : OTHER_FIRST;
        mix(*(unsigned *)(m + 0x14));
        for (unsigned p = 0; p < PASSES; p++) {
            unsigned char *l = m + at + p * LIST_SIZE;
            unsigned c = *(unsigned *)(l + 8);
            unsigned **d = *(unsigned ***)(l + 0xC);
            mix(c); mix(l[0x4C]);
            for (unsigned t = 1; t <= THREADS; t++) mix((*(unsigned **)(l + 0x44))[t]);
            for (unsigned k = 0; k < c; k++) mix(*d[k]);
        }
    }
    for (unsigned i = 0; i < s->owners; i++) {
        unsigned c = *(unsigned *)(s->owner[i] + 0x4C + 4);
        unsigned **d = *(unsigned ***)(s->owner[i] + 0x4C + 8);
        mix(c);
        for (unsigned k = 0; k < c; k++) mix(*d[k]);
    }
    return g_h;
}

static void(__thiscall *g_job)(void *self, void *node);

// the scenes: seed -> shape. Small ones stay under the item limit, some have no items or one mesh, a few are big.
#define SCENES 60
static unsigned long long run_scene(unsigned seed) {
    struct scene s;
    int kind = seed % 10 == 7 ? OTHER_CLASS : seed % 10 == 8 ? MIXED : SOLDIERS;
    unsigned meshes = seed % 12 == 0 ? 1 : seed % 12 == 1 ? 2 : 3 + seed * 7 % 120;
    unsigned items = seed % 15 == 2 ? 0 : 500 + seed * 131 % 9000;
    build(&s, seed, kind, meshes, items, seed % 6 == 3 ? 4000 : 0);
    g_job(NULL, s.node);
    return result(&s);
}

static unsigned long long g_ref[SCENES];
static volatile LONG g_bad;
static DWORD WINAPI second_job(void *x) {   // another soldier node on another thread, while the first one runs
    t_arena = VirtualAlloc(NULL, ARENA, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    for (unsigned r = 0; r < 3; r++)
        for (unsigned seed = 0; seed < SCENES; seed++)
            if (run_scene(seed) != g_ref[seed]) InterlockedIncrement(&g_bad);
    (void)x;
    return 0;
}

static unsigned char *call_target(unsigned char *site) { CHECK(site[0] == 0xE8); return site + 5 + *(LONG *)(site + 1); }
static void jump_to(unsigned char *at, void *to) {
    DWORD old;
    CHECK(VirtualProtect(at, 5, PAGE_EXECUTE_READWRITE, &old));
    at[0] = 0xE9;
    LONG rel = (LONG)((unsigned char *)to - (at + 5));
    memcpy(at + 1, &rel, 4);
}
static DWORD g_end_args[5];
static void __thiscall end_dummy(void *self, DWORD a, DWORD b, DWORD c, DWORD d) {
    g_end_args[0] = (DWORD)(UINT_PTR)self; g_end_args[1] = a; g_end_args[2] = b; g_end_args[3] = c; g_end_args[4] = d;
}

int main(int argc, char **argv) {
    CHECK(argc == 2);
    wchar_t w[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, argv[1], -1, w, MAX_PATH);
    HMODULE m = LoadLibraryExW(w, NULL, DONT_RESOLVE_DLL_REFERENCES);
    CHECK(m != NULL);
    unsigned char *b = (unsigned char *)m;
    t_arena = VirtualAlloc(NULL, ARENA, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    CHECK(t_arena != NULL);
    int k = -1;
    for (int i = 0; i < NBUILDS && k < 0; i++)
        if (fnv1a_unreloc(b, g_ps_builds[i][0], LEN_BLOCK) == g_ps_builds[i][1]) k = i;
    CHECK(k >= 0);
    static const DWORD node_fn[NBUILDS] = {0x14ADAC0, 0x14872C0, 0x14AD8C0};   // the one-node function, per build
    const DWORD *e = g_ps_builds[k];
    unsigned char *blk = b + e[0], *sol = b + e[2], *merge = b + e[4], *node_site = b + node_fn[k] + 0x43;
    printf("build %d: block +0x%lX, soldier function +0x%lX, merge +0x%lX, one-node function +0x%lX\n", k, e[0], e[2], e[4], node_fn[k]);
    CHECK(call_target(node_site) == call_target(blk + AT_MESH_SORT));   // both call the mesh sort
    CHECK(call_target(sol + 0x18) == merge);
    // the game's allocator (the merge's calls) and free: ours instead
    unsigned char *alloc = call_target(merge + 0x5F), *release = call_target(merge + 0x85);
    jump_to(alloc, (void *)game_new);
    jump_to(release, (void *)game_delete);
    g_job = (void(__thiscall *)(void *, void *))(void *)(b + node_fn[k]);
    // another mesh class: lists at +0x214, the second sort (+0x44/+0x40), no append
    static const unsigned char other_head[] = {0x56, 0x57, 0x6B, 0x7C, 0x24, 0x0C, 0x54, 0x03, 0xF9, 0x8D, 0x8F, 0x14, 0x02, 0x00, 0x00};
    unsigned char *other = sol - 0x170;
    CHECK(memcmp(other, other_head, sizeof other_head) == 0 && call_target(other + 0xF) == merge);
    g_vt_soldier[0xE8 / 4] = g_vt_other[0xE8 / 4] = (void *)mesh_key;
    g_vt_soldier[VT_LISTS / 4] = sol;
    g_vt_other[VT_LISTS / 4] = other;

    CHECK(install_parsort(m, 0, say) == 0);   // not without the draw-list sort
    CHECK(install_sortfix(m, say) == 2);
    CHECK(call_target(other + 0x2E) == b + g_sorts[1].rva);
    // the reference: the game's node, with the two sorts of sortfix.c (the order among equal keys is theirs)
    for (unsigned seed = 0; seed < SCENES; seed++) g_ref[seed] = run_scene(seed);
    for (unsigned seed = 0; seed < SCENES; seed++) CHECK(run_scene(seed) == g_ref[seed]);   // and it repeats
    {   // the reference is a sorted, merged, appended scene at all
        struct scene s;
        build(&s, 5, SOLDIERS, 40, 3000, 0);
        g_job(NULL, s.node);
        unsigned lists = 0, appended = 0, master = 0;
        for (unsigned i = 0; i < s.meshes; i++)
            for (unsigned p = 0; p < PASSES; p++) {
                unsigned char *l = s.mesh[i] + LIST_FIRST + p * LIST_SIZE;
                unsigned c = *(unsigned *)(l + 8);
                void **d = *(void ***)(l + 0xC);
                CHECK(!l[0x4C]);
                for (unsigned j = 1; j < c; j++) CHECK(key_of(d[j - 1]) <= key_of(d[j]));
                lists += c > 1;
                appended += c;
            }
        for (unsigned i = 0; i < s.owners; i++) master += *(unsigned *)(s.owner[i] + 0x4C + 4);
        CHECK(lists > 100 && appended == master && *(unsigned *)(s.node + 0x94 + (PASSES - 1) * 4) == appended);
    }

    g_parsort_helpers = 3;
    g_parsort_min = 64;
    CHECK(install_parsort(m, 1, say) == 1);   // with the A/B: the block's end call too
    g_parsort_ab = 0;
    CHECK(call_target(blk + AT_MESH_SORT) == (unsigned char *)(void *)ps_mesh_sort);
    CHECK(call_target(blk + AT_BLOCK_END) == (unsigned char *)(void *)ps_block_end);
    CHECK(call_target(sol + AT_LIST_SORT) == (unsigned char *)(void *)ps_list_sort);
    CHECK(install_parsort(m, 0, say) == 0);   // already patched: the bytes differ
    {   // the one-node function's mesh sort call: the same wrapper, so the game's loop runs it here
        DWORD old;
        CHECK(VirtualProtect(node_site, 5, PAGE_EXECUTE_READWRITE, &old));
        LONG rel = (LONG)((unsigned char *)(void *)ps_mesh_sort - (node_site + 5));
        memcpy(node_site + 1, &rel, 4);
    }
    static const int helpers[] = {3, 1, 2, 3};
    for (unsigned h = 0; h < 4; h++) {
        g_parsort_helpers = helpers[h];
        LONG before = g_parsort_jobs;
        for (unsigned r = 0; r < 4; r++)
            for (unsigned seed = 0; seed < SCENES; seed++) CHECK(run_scene(seed) == g_ref[seed]);
        CHECK(g_parsort_jobs - before >= 4 * 30);   // most scenes went the new way
    }
    printf("%d scenes x 4 rounds with 1, 2 and 3 helper threads: every list, master list and total as on one thread\n", SCENES);
    LONG before = g_parsort_jobs;
    g_parsort_min = 1u << 30;   // too few items: left to the game's loop
    for (unsigned seed = 0; seed < SCENES; seed++) CHECK(run_scene(seed) == g_ref[seed]);
    g_parsort_min = 64;
    g_parsort_on = 0;           // the A/B switch
    for (unsigned seed = 0; seed < SCENES; seed++) CHECK(run_scene(seed) == g_ref[seed]);
    g_parsort_on = 1;
    g_parsort_helpers = 0;      // no helpers
    for (unsigned seed = 0; seed < SCENES; seed++) CHECK(run_scene(seed) == g_ref[seed]);
    g_parsort_helpers = 3;
    sortfix_enable(0);          // sortfix.c's A/B puts the game's sort back: the game's loop sorts
    CHECK(!draw_sort_is_ours());
    for (unsigned seed = 0; seed < SCENES; seed++) run_scene(seed);
    sortfix_enable(1);
    CHECK(g_parsort_jobs == before);
    {   // a node of another mesh class and a mixed one are never sorted here
        struct scene s;
        build(&s, 7, OTHER_CLASS, 30, 2000, 0); g_job(NULL, s.node);
        build(&s, 8, MIXED, 30, 2000, 0); g_job(NULL, s.node);
        CHECK(g_parsort_jobs == before);
    }
    // many small frames in a row: a helper that wakes late meets the next frame's tasks. With the A/B of parsort_ab.txt
    // on: whichever window the clock is in
    g_parsort_ab = 1;
    for (unsigned r = 0; r < 5000; r++) CHECK(run_scene(20 + r % 5) == g_ref[20 + r % 5]);
    // the block's end call: the game's arguments go through, and every 4096 soldier blocks one line to the log
    g_block_end = end_dummy;
    g_par_log = say_count;
    for (unsigned r = 0; r < 4096; r++) {
        ps_block_end((void *)0x1234, r, 2, 3, 4);
        CHECK(g_end_args[0] == 0x1234 && g_end_args[1] == r && g_end_args[2] == 2 && g_end_args[3] == 3 && g_end_args[4] == 4);
        CHECK(t_count == 0);
    }
    CHECK(g_said == 1);
    g_par_log = say;
    g_parsort_ab = 0;
    g_parsort_on = 1;
    // two soldier nodes at once on two threads: one gets the helpers, the other runs as the game does
    HANDLE t = CreateThread(NULL, 0, second_job, NULL, 0, NULL);
    CHECK(t != NULL);
    for (unsigned r = 0; r < 3; r++)
        for (unsigned seed = 0; seed < SCENES; seed++) CHECK(run_scene(seed) == g_ref[seed]);
    WaitForSingleObject(t, INFINITE);
    CHECK(g_bad == 0);
    printf("small frames in a row, two nodes at once, the item limit, the switches, other mesh classes: same results\n");

    // speed: a frame like a big battle (30,000 draw items, each in about 6 lists)
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    // the two ways take turns frame by frame, so whatever else the PC is doing meets both alike; the median says what a
    // frame costs, the worst one whether a helper ever held the node up
    double us[2][30], med[2], worst[2];
    for (int r = 0; r < 30; r++)
        for (int on = 0; on < 2; on++) {
            struct scene s;
            g_parsort_on = (unsigned char)on;
            build(&s, 1000 + r, SOLDIERS, 600, 30000, 0);
            QueryPerformanceCounter(&t0);
            g_job(NULL, s.node);
            QueryPerformanceCounter(&t1);
            us[on][r] = (t1.QuadPart - t0.QuadPart) * 1e6 / f.QuadPart;
        }
    g_parsort_on = 1;
    for (int on = 0; on < 2; on++) {
        for (int i = 1; i < 30; i++)
            for (int j = i; j && us[on][j - 1] > us[on][j]; j--) { double t2 = us[on][j]; us[on][j] = us[on][j - 1]; us[on][j - 1] = t2; }
        med[on] = us[on][15];
        worst[on] = us[on][29];
    }
    printf("a frame of 600 meshes, median (worst of 30): the node on one thread %.0f us (%.0f), with 3 helpers %.0f us (%.0f): %.2fx\n",
           med[0], worst[0], med[1], worst[1], med[0] / med[1]);
    printf("all parallel list sort tests passed\n");
    return 0;
}
