// Ported from the Attila mod's sortfix.c (same code, Rome 2 addresses and keys). Faster replacements for two
// render-list sorts. Rome 2 +0x14A8370 (= Attila FUN_117ad5d0, identical but for 5 call-target bytes) and +0x14A7930
// (= Attila FUN_117acb50 with other key offsets) are std::sort (introsort) instantiations over pointers to render
// items, ordered ascending by two unsigned keys read through the pointers: +0x6C then +0x68 (the draw list) and +0x44
// then +0x40. In Rome 2 they run on the main thread (battle 2.2%, campaign ~1% of it). Every comparison reads both keys through the pointers, so in big battles they miss the cache constantly, and
// the main thread waits for them. This reads each item's keys once into a compact array, radix-sorts that (stable:
// equal keys keep their input order; the originals left their order unspecified) and writes the pointers back.
#include <windows.h>
#include <string.h>
#include "builds.h"

#define DRAWSORT_LEN 630

struct kp { unsigned long long key; void *p; };

static __thread struct kp *t_a, *t_b;
static __thread unsigned t_cap;

typedef void(__cdecl *sort_fn)(void **, void **, int, void *);

static struct sort_site {
    DWORD rva, fnv;            // the build found by sortfix_find (FNV-1a of the whole 630-byte function, no relocations)
    unsigned hi, lo;           // key offsets: compared as (hi, lo)
    const char *name;
    unsigned char tramp[16];   // the overwritten prologue + jmp back: the original sort
    DWORD builds[NBUILDS][2];  // {rva, fnv} per known build (the functions differ only in their call targets)
} g_sorts[2] = {
    {0x14A8370, 0x89B4A107u, 0x6C, 0x68, "draw-list sort", {0}, {{0x14A8370, 0x89B4A107u}, {0x1481B70, 0xF11FB1D5u}, {0x14A8170, 0x89B4A107u}}},
    {0x14A7930, 0xEE40594Fu, 0x44, 0x40, "render-list sort (+0x44/+0x40)", {0}, {{0x14A7930, 0xEE40594Fu}, {0x1481130, 0xDB6F7BCCu}, {0x14A7730, 0xEE40594Fu}}},
};

static unsigned long long key_at(const void *p, const struct sort_site *s) {
    const unsigned char *o = p;
    return ((unsigned long long)*(const unsigned *)(o + s->hi) << 32) | *(const unsigned *)(o + s->lo);
}

// The draw-list key, kept for the tests.
static __attribute__((unused)) unsigned long long key_of(void *p) { return key_at(p, &g_sorts[0]); }

static int grow(unsigned n) {
    if (n <= t_cap) return 1;
    unsigned cap = n + n / 2 + 64;
    HANDLE h = GetProcessHeap();
    void *a = t_a ? HeapReAlloc(h, 0, t_a, cap * sizeof *t_a) : HeapAlloc(h, 0, cap * sizeof *t_a);
    if (!a) return 0;
    t_a = a;
    void *b = t_b ? HeapReAlloc(h, 0, t_b, cap * sizeof *t_b) : HeapAlloc(h, 0, cap * sizeof *t_b);
    if (!b) return 0;
    t_b = b;
    t_cap = cap;
    return 1;
}

static void sort_by_keys(void **first, void **last, int ideal, void *ctx, const struct sort_site *s) {
    unsigned n = (unsigned)(last - first);
    if (n < 2) return;
    if (n <= 32) {  // small: insertion sort (stable)
        for (unsigned i = 1; i < n; i++) {
            void *v = first[i];
            unsigned long long k = key_at(v, s);
            unsigned j = i;
            while (j && key_at(first[j - 1], s) > k) { first[j] = first[j - 1]; j--; }
            first[j] = v;
        }
        return;
    }
    if (!grow(n)) {
        ((sort_fn)(void *)s->tramp)(first, last, ideal, ctx);
        return;
    }
    struct kp *a = t_a, *b = t_b;
    unsigned hist[8][256];
    memset(hist, 0, sizeof hist);
    for (unsigned i = 0; i < n; i++) {
        unsigned long long k = key_at(first[i], s);
        a[i].key = k;
        a[i].p = first[i];
        for (int d = 0; d < 8; d++) hist[d][(k >> (8 * d)) & 0xFF]++;
    }
    for (int d = 0; d < 8; d++) {
        unsigned *h = hist[d];
        if (h[(a[0].key >> (8 * d)) & 0xFF] == n) continue;  // every key has the same digit here
        unsigned sum = 0;
        for (int i = 0; i < 256; i++) { unsigned c = h[i]; h[i] = sum; sum += c; }
        for (unsigned i = 0; i < n; i++) b[h[(a[i].key >> (8 * d)) & 0xFF]++] = a[i];
        struct kp *t = a; a = b; b = t;
    }
    for (unsigned i = 0; i < n; i++) first[i] = a[i].p;
}

void __cdecl draw_sort(void **first, void **last, int ideal, void *ctx) { sort_by_keys(first, last, ideal, ctx, &g_sorts[0]); }
void __cdecl list_sort(void **first, void **last, int ideal, void *ctx) { sort_by_keys(first, last, ideal, ctx, &g_sorts[1]); }

// Points each sort at the known build whose bytes match at m (left as it is when none does).
void sortfix_find(HMODULE m) {
    for (int i = 0; i < 2; i++)
        for (int b = 0; b < NBUILDS; b++) {
            const DWORD *e = g_sorts[i].builds[b];
            if (in_image(m, e[0], DRAWSORT_LEN) && fnv1a((unsigned char *)m + e[0], DRAWSORT_LEN) == e[1]) {
                g_sorts[i].rva = e[0];
                g_sorts[i].fnv = e[1];
                break;
            }
        }
}

static unsigned char g_sort_orig[2][5], g_sort_new[2][5];
static HMODULE g_sort_m;
static int g_sort_n;

// A/B test switch (main thread, frame start: the sorts only run on the main thread in Rome 2).
void sortfix_enable(int on) {
    for (int i = 0; i < 2 && g_sort_n == 2; i++) {
        unsigned char *site = (unsigned char *)g_sort_m + g_sorts[i].rva;
        DWORD old;
        if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) return;
        memcpy(site, on ? g_sort_new[i] : g_sort_orig[i], 5);
        VirtualProtect(site, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), site, 5);
    }
}

// Returns how many of the two sorts were redirected (each is independent: either one alone is correct).
int install_sortfix(HMODULE m, void (*log)(const char *fmt, ...)) {
    void *repl[2] = {(void *)draw_sort, (void *)list_sort};
    int n = 0;
    sortfix_find(m);
    for (int i = 0; i < 2; i++) {
        struct sort_site *s = &g_sorts[i];
        unsigned char *site = (unsigned char *)m + s->rva;
        if (fnv1a(site, DRAWSORT_LEN) != s->fnv) {
            log("%s at +0x%X does not match this game version - not patched", s->name, s->rva);
            continue;
        }
        // Trampoline = the first 5 bytes (sub esp,14h; push ebx; push ebp) + jmp to site+5.
        DWORD old;
        if (!VirtualProtect(s->tramp, sizeof s->tramp, PAGE_EXECUTE_READWRITE, &old)) continue;
        memcpy(s->tramp, site, 5);
        s->tramp[5] = 0xE9;
        DWORD rel = (DWORD)((site + 5) - (s->tramp + 10));
        memcpy(s->tramp + 6, &rel, 4);
        FlushInstructionCache(GetCurrentProcess(), s->tramp, sizeof s->tramp);
        if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) {
            log("%s: VirtualProtect failed (%lu) - not patched", s->name, GetLastError());
            continue;
        }
        memcpy(g_sort_orig[i], site, 5);
        site[0] = 0xE9;
        rel = (DWORD)((unsigned char *)repl[i] - (site + 5));
        memcpy(site + 1, &rel, 4);
        memcpy(g_sort_new[i], site, 5);
        VirtualProtect(site, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), site, 5);
        log("%s replaced (key extraction + radix sort)", s->name);
        n++;
    }
    if (n) {  // a refused second install must not reset the A/B switch
        g_sort_m = m;
        g_sort_n = n;
    }
    return n;
}
