// Faster tile-feedback scan for Total War: ROME 2 (empire.retail.dll, builds in builds.h).
// Every frame the main thread reads back a screen-sized GPU buffer of 32-bit tile requests
// (12-bit x, 12-bit y, layer byte; layer 0 = none) in +0x1594020 (pantheon +0x156DCF0) and, pixel by pixel, does a
// saturating (0xFFFE) increment of that tile's 16-bit counter, appending the counter's address to
// a list the first time it becomes 1. That loop was ~1/3 of the main thread's game code on the
// campaign map and in battle. Neighbouring pixels nearly always request the same tile, so this
// version counts each run of identical entries once: same final counters, same list, same order.
// Only the two per-pixel loops are replaced (jmp at their entry, back to the original tail);
// both ranges contain no relocated addresses, so a plain hash of their bytes identifies them.
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "builds.h"

// Offsets from the function's entry (the same in every known build).
#define OFF_LOOP_A 0x45u   // entry layout: x = (b2 & 0xF) << 8 | b0, y = (b2 & 0xF0) << 4 | b1
#define OFF_LOOP_B 0x178u  // entry layout: x = (b0 & 0xF) << 8 | b2, y = (b0 & 0xF0) << 4 | b1
#define LEN_A 0xD6u        // up to the tail at OFF_TAIL_A
#define LEN_B 0xE3u
#define OFF_TAIL_A 0x117u  // mov ebx,[esp+24h] then the original tail
#define OFF_TAIL_B 0x257u
#define LEN_PUSH 0x21u
#define HASH_PUSH 0xDF48CE41u

// Per build: the function's entry, the game's vector<u16*>::push_back (thiscall, 1 arg: pointer to value), and the
// hashes of the two loops (they differ between builds only in the rel32 of their call to push_back).
static const struct ts_build { DWORD entry, push, hash_a, hash_b; } g_ts_builds[NBUILDS] = {
    {0x1594020u, 0xE89B0u, 0xFE3AA4B9u, 0x2F13EBF9u},
    {0x156DCF0u, 0xEAEF0u, 0x8E692C80u, 0xA3DF4B93u},
};

typedef struct { uint16_t *counts; uint32_t width, height, unused; } Layer;
typedef struct { Layer *layers; uint32_t nlayers; uint32_t list[3]; } Grid;  // list = game vector at +8
typedef void(__thiscall *push_fn)(void *vec, uint16_t **value);

static push_fn g_push;
void *g_tail_a, *g_tail_b;  // read by the thunks
static uint32_t g_ts_pixels __attribute__((unused)), g_ts_runs, g_ts_calls;  // stats (logged by the profiling build), main thread only
static void (*g_log)(const char *fmt, ...);

static uint32_t fnv1a(const unsigned char *p, size_t n) {
    uint32_t h = 0x811C9DC5u;
    while (n--) h = (h ^ *p++) * 0x01000193u;
    return h;
}

// Applies `run` consecutive hits of one entry exactly as the original per-pixel loop would.
static void hit(Grid *g, uint32_t v, uint32_t run, int fmt_b) {
    uint32_t layer = v >> 24;
    if (layer - 1 >= g->nlayers) return;  // also rejects layer 0
    uint32_t b0 = v & 0xFF, b1 = (v >> 8) & 0xFF, b2 = (v >> 16) & 0xFF;
    uint32_t x = fmt_b ? (b0 & 0xF) << 8 | b2 : (b2 & 0xF) << 8 | b0;
    uint32_t y = fmt_b ? (b0 & 0xF0) << 4 | b1 : (b2 & 0xF0) << 4 | b1;
    Layer *l = &g->layers[layer - 1];
    if (x >= l->width || y >= l->height) return;
    uint16_t *c = &l->counts[y * l->width + x];
    uint32_t old = *c;
    // per hit: c = min(c + 1, 0xFFFE); 0xFFFF also drops to 0xFFFE
    *c = (uint16_t)(old >= 0xFFFE || run >= 0xFFFEu - old ? 0xFFFE : old + run);
    if (old == 0) g_push(&g->list, &c);  // only the first hit of a fresh counter yields 1
}

static void scan(Grid *g, const unsigned char *rowp, uint32_t cols, uint32_t rows, uint32_t pitch_px, int fmt_b) {
    uint32_t prev = 0, run = 0;  // prev 0 = layer 0, ignored
    for (uint32_t r = 0; r < rows; r++, rowp += pitch_px * 4) {
        const uint32_t *p = (const uint32_t *)rowp;
        for (uint32_t i = 0; i < cols; i++) {
            if (p[i] == prev) { run++; continue; }
            if (run && prev) { hit(g, prev, run, fmt_b); g_ts_runs++; }
            prev = p[i];
            run = 1;
        }
    }
    if (run && prev) { hit(g, prev, run, fmt_b); g_ts_runs++; }
    g_ts_pixels += rows * cols;
    if (++g_ts_calls % 4096 == 0) {  // roughly once a minute: shows how much the run-length trick saves
#ifdef PROFILING  // measuring only: noise in the players' log
        g_log("tile feedback scan: %ux%u entries, %.1f entries per run over the last 4096 frames", cols, rows,
              g_ts_runs ? (double)g_ts_pixels / g_ts_runs : 0.0);
#endif
        g_ts_pixels = g_ts_runs = 0;
    }
}

// frame = esp at the loop entry: [10h] cols (A) / rows (B), [14h] buffer, [18h] cols (B), [1Ch] pitch in pixels, [28h] rows (both)
void __cdecl tilescan_a(Grid *g, const unsigned char *frame) {
    scan(g, *(const unsigned char **)(frame + 0x14), *(const uint32_t *)(frame + 0x10), *(const uint32_t *)(frame + 0x28),
         *(const uint32_t *)(frame + 0x1C), 0);
}
void __cdecl tilescan_b(Grid *g, const unsigned char *frame) {
    scan(g, *(const unsigned char **)(frame + 0x14), *(const uint32_t *)(frame + 0x18), *(const uint32_t *)(frame + 0x28),
         *(const uint32_t *)(frame + 0x1C), 1);
}

// esi = Grid*, esp = the original frame; C code keeps esi/edi/ebx/ebp, the tail only needs esi and esp.
// The entry thunk counts frames, then runs the function's first 8 bytes (copied, position independent).
__asm__(".globl _tilescan_thunk_a\n_tilescan_thunk_a:\n"
        "  mov %esp, %eax\n  push %eax\n  push %esi\n  call _tilescan_a\n  add $8, %esp\n  jmp *_g_tail_a\n"
        ".globl _tilescan_thunk_b\n_tilescan_thunk_b:\n"
        "  mov %esp, %eax\n  push %eax\n  push %esi\n  call _tilescan_b\n  add $8, %esp\n  jmp *_g_tail_b\n"
        ".globl _tilescan_thunk_entry\n_tilescan_thunk_entry:\n"
        "  push %ecx\n  call _tilescan_entry\n  pop %ecx\n"
        "  sub $0x10, %esp\n  push %ebx\n  mov 0x18(%esp), %ebx\n  jmp *_g_entry_back\n");
void tilescan_thunk_a(void);
void tilescan_thunk_b(void);
void tilescan_thunk_entry(void);

static const unsigned char ENTRY_BYTES[8] = {0x83, 0xEC, 0x10, 0x53, 0x8B, 0x5C, 0x24, 0x18};  // sub esp,10h; push ebx; mov ebx,[esp+18h]
void *g_entry_back;
static unsigned char *g_loop_a, *g_loop_b, g_orig[2][5], g_new[2][5];
static int g_ab, g_on, g_fpslog;
void (*g_frame_check)(void);  // per-frame callback on the main thread (x87fix_frame)
void (*g_ab_flip)(int on);    // A/B test of another fix: flipped instead of the tile scan
const char *g_ab_name = "tile scan";
static int g_ab_on = 1;
static DWORD g_tid, g_t0, g_frames;

// Puts the original (on = 0) or patched (on = 1) loop entries in place. Only call it while no thread
// is inside the loops (install time, or at the function's entry on the thread that runs it).
void tilescan_enable(int on) {
    DWORD old;
    if (!VirtualProtect(g_loop_a, OFF_LOOP_B + 5 - OFF_LOOP_A, PAGE_EXECUTE_READWRITE, &old)) return;
    memcpy(g_loop_a, on ? g_new[0] : g_orig[0], 5);
    memcpy(g_loop_b, on ? g_new[1] : g_orig[1], 5);
    VirtualProtect(g_loop_a, OFF_LOOP_B + 5 - OFF_LOOP_A, old, &old);
    FlushInstructionCache(GetCurrentProcess(), g_loop_a, OFF_LOOP_B + 5 - OFF_LOOP_A);
    g_on = on;
}

// Runs once per frame on the main thread. Logs frames per second every 10 s; in A/B mode it also
// flips between the game's loop and ours at each 10 s boundary.
void __cdecl tilescan_entry(void) {
    DWORD tid = GetCurrentThreadId(), now = GetTickCount();
    if (!g_tid) g_tid = tid;
    if (tid != g_tid) {  // a second thread: flipping code under it would be unsafe
        if (g_ab) { g_ab = 0; tilescan_enable(1); g_log("tile feedback scan: called from a second thread - A/B mode stopped, fix stays on"); }
        return;
    }
    if (g_frame_check) g_frame_check();
    g_frames++;
    if (!g_t0) g_t0 = now;
    if (now - g_t0 < 10000) return;
    if (g_ab_flip) {
        g_log("A/B: %s %s  %.1f fps", g_ab_name, g_ab_on ? "ON " : "OFF", g_frames * 1000.0 / (now - g_t0));
        g_ab_on = !g_ab_on;
        g_ab_flip(g_ab_on);
    } else if (g_ab) {
        g_log("A/B: fix %s  %.1f fps", g_on ? "ON " : "OFF", g_frames * 1000.0 / (now - g_t0));
        tilescan_enable(!g_on);
    } else if (g_fpslog)
        g_log("fps %.1f", g_frames * 1000.0 / (now - g_t0));
    g_t0 = now;
    g_frames = 0;
}

// Always installs the per-frame entry hook (frame counter, g_frame_check). mode 0: loops stay original,
// 1: fix on, 2: flip the fix on and off every 10 s and log the frame rate of each window.
// fpslog: log the frame rate every 10 s.
// The known build whose bytes are all in place at m, or NULL.
const struct ts_build *tilescan_find(HMODULE m) {
    const unsigned char *base = (const unsigned char *)m;
    static const unsigned char tail[4] = {0x8B, 0x5C, 0x24, 0x24};
    for (int i = 0; i < NBUILDS; i++) {
        const struct ts_build *b = &g_ts_builds[i];
        const unsigned char *e = base + b->entry;
        if (in_image(m, b->entry, OFF_TAIL_B + 4) && in_image(m, b->push, LEN_PUSH) &&
            fnv1a(e + OFF_LOOP_A, LEN_A) == b->hash_a && fnv1a(e + OFF_LOOP_B, LEN_B) == b->hash_b &&
            fnv1a(base + b->push, LEN_PUSH) == HASH_PUSH && !memcmp(e + OFF_TAIL_A, tail, 4) &&
            !memcmp(e + OFF_TAIL_B, tail, 4) && !memcmp(e, ENTRY_BYTES, 8))
            return b;
    }
    return NULL;
}

int install_tilescan(HMODULE m, int mode, int fpslog, void (*log)(const char *fmt, ...)) {
    int ab = mode == 2;
    unsigned char *base = (unsigned char *)m;
    const struct ts_build *b = tilescan_find(m);
    if (!b) {
        log("tile feedback scan: code does not match this game version - not patched");
        return 0;
    }
    unsigned char *entry = base + b->entry;
    g_log = log;
    g_loop_a = entry + OFF_LOOP_A;
    g_loop_b = entry + OFF_LOOP_B;
    g_ab = ab;
    g_fpslog = fpslog;
    g_push = (push_fn)(base + b->push);
    g_tail_a = entry + OFF_TAIL_A;
    g_tail_b = entry + OFF_TAIL_B;
    g_entry_back = entry + 8;
    void *to[2] = {(void *)tilescan_thunk_a, (void *)tilescan_thunk_b};
    unsigned char *loop[2] = {g_loop_a, g_loop_b};
    for (int i = 0; i < 2; i++) {
        memcpy(g_orig[i], loop[i], 5);
        g_new[i][0] = 0xE9;
        int32_t rel = (int32_t)((unsigned char *)to[i] - (loop[i] + 5));
        memcpy(g_new[i] + 1, &rel, 4);
    }
    DWORD old;
    if (!VirtualProtect(entry, 8, PAGE_EXECUTE_READWRITE, &old)) {
        log("tile feedback scan: VirtualProtect failed (%lu) - not patched", GetLastError());
        return 0;
    }
    tilescan_enable(1);
    if (!g_on) {
        VirtualProtect(entry, 8, old, &old);
        log("tile feedback scan: VirtualProtect failed (%lu) - not patched", GetLastError());
        return 0;
    }
    int32_t rel = (int32_t)((unsigned char *)tilescan_thunk_entry - (entry + 5));
    unsigned char jmp[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
    memcpy(jmp + 1, &rel, 4);
    memcpy(entry, jmp, 8);
    VirtualProtect(entry, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), entry, 8);
    if (!mode) tilescan_enable(0);
    log(ab ? "tile feedback scan replaced, A/B mode: fix flips on/off every 10 s, fps logged per window"
           : mode ? "tile feedback scan replaced (run-length counting)" : "tilescan_off.txt found - tile feedback scan left original (frame hook only)");
    return 1;
}
