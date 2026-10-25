// Differential test: the game's string equality (+0xF41F0), compare (+0xF9AC0) and assign (+0xF0380) vs strfix.c's
// replacements on random strings (shared prefixes, bytes >= 0x80, lengths around 4/16, strings ending at a no-access
// page), plus speed. assign's original runs from a copy of its bytes with the allocator call and the two address
// operands pointed at this test's own allocator and data.
// usage: strfix_test.exe <path to empire.retail.dll>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "../strfix.c"

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static void say(const char *fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); putchar('\n'); }
static unsigned rng = 7;
static unsigned rnd(unsigned n) { rng = rng * 1103515245u + 12345u; return (rng >> 8) % n; }

typedef unsigned char(__cdecl *eq_fn)(const gstr *, const gstr *);
typedef int(__thiscall *cmp_fn)(const gstr *, const gstr *);
typedef void(__thiscall *as_fn)(gstr *, const char *, const char *);

static char t_empty[4] = "", t_term = 0;
static char *__cdecl t_alloc(unsigned n, unsigned z) { (void)z; char *p = malloc(n); memset(p, 0xCD, n); return p; }

static char *g_freed[4];
static int g_nfree;
static void __thiscall t_free(gstr *s) { if (g_nfree < 4) g_freed[g_nfree] = s->p; g_nfree++; }

// a string whose last byte sits right before a no-access page (catches reads past the end)
static char *g_page;
static char *at_page_end(const char *src, unsigned n) {
    char *p = g_page + 4096 - n;
    memcpy(p, src, n);
    return p;
}

static void fill(char *b, unsigned n, int high) { for (unsigned i = 0; i < n; i++) b[i] = (char)(high ? rnd(256) : 'a' + rnd(3)); }

int main(int argc, char **argv) {
    CHECK(argc == 2);
    wchar_t w[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, argv[1], -1, w, MAX_PATH);
    HMODULE m = LoadLibraryExW(w, NULL, DONT_RESOLVE_DLL_REFERENCES);
    CHECK(m != NULL);
    for (int i = 0; i < NSITES; i++) say("%s +0x%X hash 0x%08lX", g_str[i].name, g_str[i].rva, str_site_hash((unsigned char *)m + g_str[i].rva, i));
    for (int i = 0; i < NSITES; i++) CHECK(site_ok((unsigned char *)m + g_str[i].rva, i));

    eq_fn oeq = (eq_fn)((char *)m + RVA_EQUAL);
    cmp_fn ocmp = (cmp_fn)((char *)m + RVA_COMPARE);
    // the original assign, relocated into this test
    unsigned char *code = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(code, (char *)m + RVA_ASSIGN, 111);
    *(char **)(code + AS_EMPTY) = t_empty;
    *(LONG *)(code + AS_CALL) = (LONG)((unsigned char *)t_alloc - (code + AS_CALL + 4));
    *(char **)(code + AS_TERM) = &t_term;
    as_fn oas = (as_fn)code;
    g_alloc = t_alloc; g_empty = t_empty; g_term = &t_term;

    g_page = VirtualAlloc(NULL, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    DWORD old;
    CHECK(VirtualProtect(g_page + 4096, 4096, PAGE_NOACCESS, &old));
    char *page2 = VirtualAlloc(NULL, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    CHECK(VirtualProtect(page2 + 4096, 4096, PAGE_NOACCESS, &old));

    static char A[400], B[400];
    long cases = 0;
    for (int it = 0; it < 400000; it++) {
        unsigned n1 = rnd(5) == 0 ? rnd(300) : rnd(40), n2 = rnd(3) == 0 ? n1 : rnd(5) == 0 ? rnd(300) : rnd(40);
        int high = rnd(2);
        fill(A, n1, high);
        memcpy(B, A, n2 < n1 ? n2 : n1);  // shared prefix
        if (n2 > n1) fill(B + n1, n2 - n1, high);
        if (rnd(2) && n2) B[rnd(n2)] = (char)rnd(256);  // maybe one difference
        gstr a = {n1, n1, A}, b = {n2, n2, B};
        if (rnd(4) == 0) { a.p = at_page_end(A, n1); b.p = page2 + 4096 - n2; memcpy(b.p, B, n2); }
        if (rnd(50) == 0) { b = a; }  // same data pointer
        unsigned char e1 = oeq(&a, &b) & 1, e2 = str_equal(&a, &b);
        CHECK(e1 == e2);
        int c1 = ocmp(&a, &b), c2 = str_compare(&a, &b);
        if (c1 != c2) { say("compare n1=%u n2=%u orig %d mine %d", n1, n2, c1, c2); exit(1); }
        // assign: copy [b, e) with a length that may differ from e-b (the callers set len first)
        unsigned len = rnd(8) == 0 ? 0 : n1, k = rnd(8) == 0 ? (len ? rnd(len) : 0) : len;
        const char *src = a.p, *end = rnd(30) == 0 ? src - 1 : src + k;  // e < b copies nothing
        gstr s1 = {len, 77, NULL}, s2 = {len, 77, NULL};
        oas(&s1, src, end);
        str_assign(&s2, src, end);
        CHECK(s1.len == s2.len && s1.cap == s2.cap);
        if (!len) CHECK(s1.p == t_empty && s2.p == t_empty);
        else {
            CHECK(((UINT_PTR)s1.p & 3) == 0 && ((UINT_PTR)s2.p & 3) == 0);
            CHECK(((char **)s1.p)[-1] + 8 >= s1.p - 3 && ((char **)s2.p)[-1] + 8 >= s2.p - 3);
            CHECK(memcmp(s1.p, s2.p, len + 1) == 0);
            free(((char **)s1.p)[-1]); free(((char **)s2.p)[-1]);
        }
        cases++;
    }
    say("%ld random cases identical (equality, compare, assign)", cases);

    // pair search: the original from a copy, its equality calls aimed at the game's own equality, its two
    // destructor calls at a recorder (which keys get destroyed, in which order)
    unsigned char *pf = code + 512;
    memcpy(pf, (char *)m + RVA_PAIRFIND, 85);
    const int eq_calls[2] = {0x1A, 0x2C}, free_calls[2] = {0x43, 0x4C};
    for (int i = 0; i < 2; i++) *(LONG *)(pf + eq_calls[i]) = (LONG)((unsigned char *)oeq - (pf + eq_calls[i] + 4));
    for (int i = 0; i < 2; i++) *(LONG *)(pf + free_calls[i]) = (LONG)((unsigned char *)t_free - (pf + free_calls[i] + 4));
    g_str_free = t_free;
    typedef const char *(__cdecl *pf_fn)(const char *, const char *, gstr, gstr);
    pf_fn opf = (pf_fn)pf;
    static char names[64][24];
    for (int i = 0; i < 64; i++) { unsigned n = 1 + rnd(20); fill(names[i], n, rnd(2)); names[i][n] = 0; }
    static char tab[400 * 0x1C];
    long pcases = 0;
    for (int it = 0; it < 20000; it++) {
        unsigned n = rnd(400);
        for (unsigned i = 0; i < n; i++) {
            gstr *a = (gstr *)(tab + i * 0x1C), *b = (gstr *)(tab + i * 0x1C + 0xC);
            const char *x = names[rnd(rnd(2) ? 8 : 64)], *y = names[rnd(rnd(2) ? 8 : 64)];
            *a = (gstr){(unsigned)strlen(x), 0, (char *)x};
            *b = (gstr){(unsigned)strlen(y), 0, (char *)y};
            if (rnd(40) == 0) a->len = 0;
        }
        const char *x = names[rnd(rnd(2) ? 8 : 64)], *y = names[rnd(rnd(2) ? 8 : 64)];
        char kx[24], ky[24];  // keys in their own buffers (and sometimes the same pointer as an entry)
        strcpy(kx, x); strcpy(ky, y);
        gstr k1 = {(unsigned)strlen(x), 0, rnd(4) ? kx : (char *)x}, k2 = {(unsigned)strlen(y), 0, rnd(4) ? ky : (char *)y};
        if (rnd(40) == 0) k1.len = 0;
        g_nfree = 0;
        const char *r1 = opf(tab, tab + n * 0x1C, k1, k2);
        CHECK(g_nfree == 2 && g_freed[0] == k2.p && g_freed[1] == k1.p);
        g_nfree = 0;
        const char *r2 = pair_find(tab, tab + n * 0x1C, k1, k2);
        CHECK(g_nfree == 2 && g_freed[0] == k2.p && g_freed[1] == k1.p);
        CHECK(r1 == r2);
        pcases++;
    }
    say("%ld pair-search cases identical (same entry found, keys destroyed in the same order)", pcases);
    {  // speed: 300 entries, key near the end
        for (unsigned i = 0; i < 300; i++) {
            const char *x = names[i % 64], *y = names[(i / 64) % 8];
            *(gstr *)(tab + i * 0x1C) = (gstr){(unsigned)strlen(x), 0, (char *)x};
            *(gstr *)(tab + i * 0x1C + 0xC) = (gstr){(unsigned)strlen(y), 0, (char *)y};
        }
        gstr k1 = *(gstr *)(tab + 290 * 0x1C), k2 = *(gstr *)(tab + 290 * 0x1C + 0xC);
        LARGE_INTEGER f, t0, t1, t2; QueryPerformanceFrequency(&f);
        volatile UINT_PTR sink = 0;
        QueryPerformanceCounter(&t0);
        for (int i = 0; i < 20000; i++) sink += (UINT_PTR)opf(tab, tab + 300 * 0x1C, k1, k2);
        QueryPerformanceCounter(&t1);
        for (int i = 0; i < 20000; i++) sink += (UINT_PTR)pair_find(tab, tab + 300 * 0x1C, k1, k2);
        QueryPerformanceCounter(&t2);
        say("pair search, 20k x 300 entries: game %.0f ms, fix %.0f ms", (t1.QuadPart - t0.QuadPart) * 1e3 / f.QuadPart, (t2.QuadPart - t1.QuadPart) * 1e3 / f.QuadPart);
    }

    // speed: equal-length 60-byte paths that differ near the end, like asset names
    static char P[64], Q[64];
    memcpy(P, "variantmeshes/_variantmodels/man/accessories/longsword_a.vmd", 60);
    memcpy(Q, P, 60); Q[57] = 'b';
    gstr p = {60, 60, P}, q = {60, 60, Q};
    LARGE_INTEGER f, t0, t1, t2; QueryPerformanceFrequency(&f);
    volatile int sink = 0;
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < 2000000; i++) sink += oeq(&p, &q) & 1;
    QueryPerformanceCounter(&t1);
    for (int i = 0; i < 2000000; i++) sink += str_equal(&p, &q);
    QueryPerformanceCounter(&t2);
    say("equality, 2M x 60-byte: game %.0f ms, fix %.0f ms", (t1.QuadPart - t0.QuadPart) * 1e3 / f.QuadPart, (t2.QuadPart - t1.QuadPart) * 1e3 / f.QuadPart);
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < 2000000; i++) sink += ocmp(&p, &q);
    QueryPerformanceCounter(&t1);
    for (int i = 0; i < 2000000; i++) sink += str_compare(&p, &q);
    QueryPerformanceCounter(&t2);
    say("compare, 2M x 60-byte: game %.0f ms, fix %.0f ms", (t1.QuadPart - t0.QuadPart) * 1e3 / f.QuadPart, (t2.QuadPart - t1.QuadPart) * 1e3 / f.QuadPart);
    return 0;
}
