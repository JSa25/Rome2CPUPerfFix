// Differential test: the game's own tile feedback scan (+0x1594020 / pantheon +0x156DCF0, run from the real
// empire.retail.dll mapped without imports) against the run-length replacement in tilescan.c.
// In the mapping, the readback source call is redirected to a stub that hands out test buffers and
// the tail (GPU/global bookkeeping) jumps straight to the epilogue; the per-pixel loops and the
// game's push_back run unchanged. usage: tilescan_test.exe <path to empire.retail.dll>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { printf("FAILED line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

int install_tilescan(HMODULE m, int mode, int fpslog, void (*log)(const char *fmt, ...));
void tilescan_enable(int on);
struct ts_build { DWORD entry, push, hash_a, hash_b; };
const struct ts_build *tilescan_find(HMODULE m);
static void quiet(const char *fmt, ...) { (void)fmt; }

typedef struct { uint16_t *counts; uint32_t width, height, unused; } Layer;
typedef struct { Layer *layers; uint32_t nlayers; uint32_t cap, size; uint16_t **data; } Grid;
typedef void(__thiscall *scan_fn)(Grid *g, void *src, uint32_t arg2);

static uint32_t *g_buf, g_cols, g_rows, g_pitch;
static unsigned char *g_base;

// stands in for the readback source (+0x1594810 in the first build; thiscall, 4 stack args, returns the mapped buffer)
static uint32_t *__thiscall src_stub(void *self, uint32_t *cols, uint32_t *rows, uint32_t *pitch, uint32_t arg2) {
    (void)self; (void)arg2;
    *cols = g_cols; *rows = g_rows; *pitch = g_pitch;
    return g_buf;
}

static void put_rel(uint32_t rva, unsigned char op, void *to) {
    unsigned char *s = g_base + rva;
    s[0] = op;
    *(int32_t *)(s + 1) = (int32_t)((unsigned char *)to - (s + 5));
}

static uint32_t rnd(void) { static uint32_t s = 12345; s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }

static uint32_t make_entry(int nlayers, int fmt_b, int maxxy) {
    uint32_t layer = rnd() % 16 == 0 ? rnd() % 6 : 1 + rnd() % nlayers;  // sometimes 0 or out of range
    uint32_t x = rnd() % maxxy, y = rnd() % maxxy;
    uint32_t b0, b1 = y & 0xFF, b2;
    if (fmt_b) { b0 = (x >> 8 & 0xF) | (y >> 8 & 0xF) << 4; b2 = x & 0xFF; }
    else { b2 = (x >> 8 & 0xF) | (y >> 8 & 0xF) << 4; b0 = x & 0xFF; }
    return layer << 24 | b2 << 16 | b1 << 8 | b0;
}

typedef struct { Grid g; Layer l[4]; } World;

static void world_init(World *w, World *copy_from) {
    for (int i = 0; i < 4; i++) {
        Layer *s = copy_from ? &copy_from->l[i] : NULL;
        w->l[i].width = s ? s->width : 1 + rnd() % 300;
        w->l[i].height = s ? s->height : 1 + rnd() % 300;
        size_t n = (size_t)w->l[i].width * w->l[i].height;
        w->l[i].counts = malloc(n * 2);
        for (size_t k = 0; k < n; k++) {
            if (s) w->l[i].counts[k] = s->counts[k];
            else { uint32_t r = rnd() % 10; w->l[i].counts[k] = r < 6 ? 0 : r < 8 ? rnd() % 100 : 0xFFFC + rnd() % 4; }
        }
    }
    w->g.layers = w->l;
    w->g.nlayers = copy_from ? copy_from->g.nlayers : 1 + rnd() % 4;
    w->g.cap = 1 << 20; w->g.size = 0; w->g.data = malloc(sizeof(uint16_t *) << 20);
}

static void world_free(World *w) { for (int i = 0; i < 4; i++) free(w->l[i].counts); free(w->g.data); }

int main(int argc, char **argv) {
    CHECK(argc == 2);
    wchar_t path[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, argv[1], -1, path, MAX_PATH);
    HMODULE m = LoadLibraryExW(path, NULL, DONT_RESOLVE_DLL_REFERENCES);
    CHECK(m != NULL);
    const struct ts_build *b = tilescan_find(m);
    CHECK(b != NULL);
    printf("tile scan function at +0x%lX\n", b->entry);
    g_base = (unsigned char *)m + b->entry;  // offsets below are from the function entry
    DWORD old;
    CHECK(VirtualProtect((unsigned char *)m + 0xE8000, 0x1600000, PAGE_EXECUTE_READWRITE, &old));
    put_rel(0x34, 0xE8, (void *)src_stub);  // loop A's source call
    put_rel(0x167, 0xE8, (void *)src_stub);  // loop B's source call
    put_rel(0x11B, 0xE9, g_base + 0x295);  // tails -> epilogue
    put_rel(0x25B, 0xE9, g_base + 0x295);
    scan_fn orig = (scan_fn)g_base;

    unsigned char orig_a[5], orig_b[5], new_a[5], new_b[5];
    memcpy(orig_a, g_base + 0x45, 5); memcpy(orig_b, g_base + 0x178, 5);
    CHECK(install_tilescan(m, 1, 0, quiet) == 1);  // also hooks the function entry: every call below goes through it
    memcpy(new_a, g_base + 0x45, 5); memcpy(new_b, g_base + 0x178, 5);
    CHECK(new_a[0] == 0xE9 && new_b[0] == 0xE9 && g_base[0] == 0xE9);
    CHECK(install_tilescan(m, 1, 0, quiet) == 0);  // patched code no longer matches: refuses
    tilescan_enable(0);  // the A/B switch restores the game's exact bytes...
    CHECK(!memcmp(g_base + 0x45, orig_a, 5) && !memcmp(g_base + 0x178, orig_b, 5));
    tilescan_enable(1);  // ...and puts the patch back
    CHECK(!memcmp(g_base + 0x45, new_a, 5) && !memcmp(g_base + 0x178, new_b, 5));

    unsigned char src[0x40] = {0};  // the readback object: byte +4 picks the layout (non-zero = A)
    long pushes = 0, pixels = 0;
    for (int t = 0; t < 3000; t++) {
        int fmt_b = t & 1;
        src[4] = fmt_b ? 0 : 1;
        g_cols = t % 50 == 0 ? 0 : rnd() % 400;
        g_rows = t % 70 == 0 ? 0 : rnd() % 40;
        g_pitch = g_cols + rnd() % 16;
        g_buf = malloc((size_t)g_pitch * g_rows * 4 + 4);
        int maxxy = rnd() % 4 == 0 ? 4096 : 320;  // sometimes far out of bounds
        uint32_t e = 0;
        int left = 0, palette = rnd() % 3 == 0;   // palette: few distinct entries, so cells repeat
        uint32_t pal[8];
        for (int i = 0; i < 8; i++) pal[i] = make_entry(4, fmt_b, maxxy);
        for (uint32_t i = 0; i < g_pitch * g_rows; i++) {
            if (left-- <= 0) { e = rnd() % 8 == 0 ? 0 : palette ? pal[rnd() % 8] : make_entry(4, fmt_b, maxxy); left = rnd() % 40; }
            g_buf[i] = e;
        }
        World a, b;
        world_init(&a, NULL);
        world_init(&b, &a);
        tilescan_enable(0);
        orig(&a.g, src, 0);
        tilescan_enable(1);
        orig(&b.g, src, 0);
        for (int i = 0; i < 4; i++)
            CHECK(!memcmp(a.l[i].counts, b.l[i].counts, (size_t)a.l[i].width * a.l[i].height * 2));
        CHECK(a.g.size == b.g.size);
        for (uint32_t i = 0; i < a.g.size; i++) {  // same cells in the same order (same layer, same index)
            int la = -1, lb = -1;
            for (int k = 0; k < 4; k++) {
                if (a.g.data[i] >= a.l[k].counts && a.g.data[i] < a.l[k].counts + a.l[k].width * a.l[k].height) la = k;
                if (b.g.data[i] >= b.l[k].counts && b.g.data[i] < b.l[k].counts + b.l[k].width * b.l[k].height) lb = k;
            }
            CHECK(la >= 0 && la == lb && a.g.data[i] - a.l[la].counts == b.g.data[i] - b.l[lb].counts);
        }
        pushes += a.g.size; pixels += (long)g_cols * g_rows;
        world_free(&a); world_free(&b); free(g_buf);
    }
    printf("3000 random scans identical to the game's loop (%ld entries, %ld list appends, both layouts)\n", pixels, pushes);

    // Speed on a screen-like buffer: 960x540 entries; runs of ~16 (typical) and of 1 (worst case).
    for (int pass = 0; pass < 2; pass++) {
        int runlen = pass ? 1 : 16;
        g_cols = 960; g_rows = 540; g_pitch = 960;
        g_buf = malloc(960 * 540 * 4);
        src[4] = 1;
        for (uint32_t i = 0; i < 960 * 540; i++) {
            uint32_t x = (i % 960) / runlen % 256, y = (i / 960) / 4 % 256;
            g_buf[i] = 1u << 24 | (y & 0xFF) << 8 | x;
        }
        World w;
        world_init(&w, NULL);
        w.g.nlayers = 1; w.l[0].width = 256; w.l[0].height = 256;  // first layer must be big enough
        free(w.l[0].counts); w.l[0].counts = calloc(256 * 256, 2);
        double us[2];
        for (int v = 0; v < 2; v++) {
            tilescan_enable(v);
            LARGE_INTEGER f, t0, t1;
            QueryPerformanceFrequency(&f);
            QueryPerformanceCounter(&t0);
            for (int k = 0; k < 50; k++) { w.g.size = 0; orig(&w.g, src, 0); }
            QueryPerformanceCounter(&t1);
            us[v] = (t1.QuadPart - t0.QuadPart) * 1e6 / f.QuadPart / 50;
        }
        printf("960x540, runs of %2d: original %.0f us per frame, new %.0f us (%.1fx)\n", runlen, us[0], us[1], us[0] / us[1]);
        world_free(&w); free(g_buf);
    }
    printf("all tile scan tests passed\n");
    return 0;
}
