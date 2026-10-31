// Faster, result-identical replacements for three of the game's string helpers, hot while loading (normal-mod load:
// 7.8%, 3.9% and 3.3% of the main thread, self time). The game's string is {length, capacity, char *data}; all three
// walk it one byte at a time. These compare 16 bytes at a time (SSE2) and copy with memcpy.
//   +0xF41F0  bool __cdecl equal(const str *a, const str *b)          lengths equal and bytes equal
//   +0xF9AC0  int __thiscall compare(const str *this, const str *b)   first differing byte as signed char difference,
//                                                                    else -1/0/1 by length
//   +0xF0380  void __thiscall assign(str *this, const char *b, const char *e)
//             takes the length already in this->len, allocates len+9 bytes from the game's allocator, aligns to 4
//             (the raw pointer goes just below), copies e-b bytes (0 if e<b) and writes the terminator byte
#include <windows.h>
#include <string.h>
#include <emmintrin.h>
#include "builds.h"

typedef struct { unsigned len, cap; char *p; } gstr;

// index of the first differing byte, or n
static unsigned first_diff(const unsigned char *x, const unsigned char *y, unsigned n) {
    unsigned i = 0;
    for (; i + 16 <= n; i += 16) {
        unsigned m = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *)(x + i)),
                                                                 _mm_loadu_si128((const __m128i *)(y + i)))) ^ 0xFFFFu;
        if (m) return i + (unsigned)__builtin_ctz(m);
    }
    for (; i + 4 <= n; i += 4) {
        unsigned a, b;
        memcpy(&a, x + i, 4);
        memcpy(&b, y + i, 4);
        if (a != b) return i + (unsigned)__builtin_ctz(a ^ b) / 8;
    }
    for (; i < n; i++)
        if (x[i] != y[i]) return i;
    return n;
}

unsigned char __cdecl str_equal(const gstr *a, const gstr *b) {
    unsigned n = a->len;
    if (n != b->len) return 0;
    if (!n || a->p == b->p) return 1;
    return first_diff((const unsigned char *)a->p, (const unsigned char *)b->p, n) == n;
}

int __thiscall str_compare(const gstr *a, const gstr *b) {
    unsigned n1 = a->len, n2 = b->len, m = n1 < n2 ? n1 : n2;
    unsigned i = m && a->p != b->p ? first_diff((const unsigned char *)a->p, (const unsigned char *)b->p, m) : m;
    if (i < m) return (int)(signed char)a->p[i] - (int)(signed char)b->p[i];
    return n1 < n2 ? -1 : n1 > n2;
}

// the game's allocator, empty string and terminator byte, read from the patched code itself (relocated addresses)
static char *(__cdecl *g_alloc)(unsigned, unsigned);
static char *g_empty;
static const char *g_term;

void __thiscall str_assign(gstr *s, const char *b, const char *e) {
    unsigned n = s->len;
    s->cap = n;
    if (!n) { s->p = g_empty; return; }
    char *raw = g_alloc(n + 9, 0);
    char *p = (char *)(((UINT_PTR)raw + 8) & ~(UINT_PTR)3);
    ((char **)p)[-1] = raw;
    s->p = p;
    if (e > b) memcpy(p, b, (size_t)(e - b));  // fresh block: never overlaps the source
    p[s->len] = *g_term;
}

// +0x7E61B0: find in an array of 28-byte {gstr a, gstr b, flag} entries the first with b == k2 and a == k1 (two
// string keys passed by value, destroyed by the callee). The game calls the equality function twice per entry;
// in a DEI load this search was ~5.5% of the main thread. Same order, same result, lengths checked inline.
#define PF_FREE      0x43  // offset of the call rel32 to the string destructor (thiscall) inside it
static void(__thiscall *g_str_free)(gstr *);

static int same_bytes(const gstr *x, const gstr *y) {  // lengths already equal
    unsigned n = x->len;
    return !n || x->p == y->p || first_diff((const unsigned char *)x->p, (const unsigned char *)y->p, n) == n;
}

const char *__cdecl pair_find(const char *first, const char *last, gstr k1, gstr k2) {
    unsigned n1 = k1.len, n2 = k2.len;
    const char *e = first;
    for (; e != last; e += 0x1C) {
        const gstr *a = (const gstr *)e, *b = (const gstr *)(e + 0xC);
        if (b->len == n2 && a->len == n1 && same_bytes(b, &k2) && same_bytes(&k1, a)) break;
    }
    g_str_free(&k2);
    g_str_free(&k1);
    return e;
}

// shortcut: the sites are checked by FNV-1a of their bytes with address bytes zeroed (per build in the table
// below); a new game build means finding them again (tools/match_attila.py with MATCH_SRC/MATCH_DST).
struct str_site { DWORD rva, len, fnv; const char *name; void *repl; };
#define NSITES 4
static struct str_site g_str[NSITES] = {  // rva/fnv: the build found by strfix_find (default: the first one)
    {0xF41F0, 53, 0x01A19E9Au, "string equality", (void *)str_equal},
    {0xF9AC0, 164, 0xB302A678u, "string compare", (void *)str_compare},
    {0xF0380, 111, 0x16DC4500u, "string assign", (void *)str_assign},
    {0x7E61B0, 85, 0xBC18112Fu, "string pair search", (void *)pair_find},
};
// {rva, fnv} of equality, compare, assign, pair search per known build (pair search differs only in call targets)
static const DWORD g_str_builds[NBUILDS][NSITES][2] = {
    {{0xF41F0, 0x01A19E9Au}, {0xF9AC0, 0xB302A678u}, {0xF0380, 0x16DC4500u}, {0x7E61B0, 0xBC18112Fu}},
    {{0xF2250, 0x01A19E9Au}, {0xF6A10, 0xB302A678u}, {0xEF270, 0x16DC4500u}, {0x7E09C0, 0xF7CF331Fu}},
    {{0xF41F0, 0x01A19E9Au}, {0xF9AC0, 0xB302A678u}, {0xF0380, 0x16DC4500u}, {0x7E61B0, 0xBC18112Fu}},
};

static DWORD fnv_masked(const unsigned char *b, DWORD n, DWORD skip1, DWORD skip2, DWORD skip3) {
    DWORD h = 0x811C9DC5u;
    for (DWORD i = 0; i < n; i++) {
        int masked = (i >= skip1 && i < skip1 + 4) || (i >= skip2 && i < skip2 + 4) || (i >= skip3 && i < skip3 + 4);
        h = (h ^ (masked ? 0 : b[i])) * 0x01000193u;
    }
    return h;
}

// address bytes inside assign (offsets from its start): empty-string pointer, call rel32, terminator address
#define AS_EMPTY 0x0F
#define AS_CALL  0x21
#define AS_TERM  0x61
#define NOSKIP   0xFFFF

DWORD str_site_hash(const unsigned char *site, int i) {
    return i == 2 ? fnv_masked(site, 111, AS_EMPTY, AS_CALL, AS_TERM) : fnv_masked(site, g_str[i].len, NOSKIP, NOSKIP, NOSKIP);
}

static int site_ok(const unsigned char *site, int i) { return str_site_hash(site, i) == g_str[i].fnv; }

// Points the sites at the known build whose four sites all match at m (left as they are when none does).
void strfix_find(HMODULE m) {
    for (int b = 0; b < NBUILDS; b++) {
        int i = 0;
        for (; i < NSITES; i++) {
            const DWORD *e = g_str_builds[b][i];
            if (!in_image(m, e[0], g_str[i].len) || str_site_hash((unsigned char *)m + e[0], i) != e[1]) break;
        }
        if (i < NSITES) continue;
        for (i = 0; i < NSITES; i++) {
            g_str[i].rva = g_str_builds[b][i][0];
            g_str[i].fnv = g_str_builds[b][i][1];
        }
        return;
    }
}

static unsigned char g_str_orig[NSITES][5];

// All or none (each one alone would be correct, but one log line and one switch keep it simple).
int install_strfix(HMODULE m, void (*log)(const char *fmt, ...)) {
    strfix_find(m);
    for (int i = 0; i < NSITES; i++) {
        if (!site_ok((unsigned char *)m + g_str[i].rva, i)) {
            log("%s at +0x%X does not match this game version - string fix not installed", g_str[i].name, g_str[i].rva);
            return 0;
        }
    }
    unsigned char *as = (unsigned char *)m + g_str[2].rva;
    g_empty = *(char **)(as + AS_EMPTY);
    g_alloc = (char *(__cdecl *)(unsigned, unsigned))(as + AS_CALL + 4 + *(LONG *)(as + AS_CALL));
    g_term = *(const char **)(as + AS_TERM);
    unsigned char *pf = (unsigned char *)m + g_str[3].rva;
    g_str_free = (void(__thiscall *)(gstr *))(pf + PF_FREE + 4 + *(LONG *)(pf + PF_FREE));
    for (int i = 0; i < NSITES; i++) {
        unsigned char *site = (unsigned char *)m + g_str[i].rva;
        DWORD old;
        if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) {
            log("string fix: VirtualProtect failed (%lu)", GetLastError());
            for (int j = 0; j < i; j++) {  // undo the ones already patched
                unsigned char *s2 = (unsigned char *)m + g_str[j].rva;
                if (VirtualProtect(s2, 5, PAGE_EXECUTE_READWRITE, &old)) { memcpy(s2, g_str_orig[j], 5); VirtualProtect(s2, 5, old, &old); }
            }
            return 0;
        }
        memcpy(g_str_orig[i], site, 5);
        site[0] = 0xE9;
        DWORD rel = (DWORD)((unsigned char *)g_str[i].repl - (site + 5));
        memcpy(site + 1, &rel, 4);
        VirtualProtect(site, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), site, 5);
    }
    log("string fix installed: equality, compare, assign and pair search use 16-byte compares / memcpy");
    return NSITES;
}
