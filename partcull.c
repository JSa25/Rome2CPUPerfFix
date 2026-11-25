// Unit detail cull (from Attila's heapfix/partcull.c): a soldier's small parts (belts, quivers, gauntlets, bridles) are
// not turned into draw items once they would be only a few pixels tall. The engine culls per model, never per part, so
// every piece of a visible soldier goes to every view he is in. In Attila (1212 AD battle, 4K, camera still) this took
// 81.3 -> 103.5 fps; Rome 2 is not measured yet.
//
// Where: WARSCAPE::RIGID_SCENE_NODE_V2::add_instance (Steam +0x14DA460; vtable 0x119347C0 slot 29, Attila's 0x17F61A0 at
// slot 30). Same code as Attila's, instruction for instruction through the detail-level pick (capstone 2026-11-23): the
// metric (node[0xA58] * distance)^2 + node[0xA5C] + params[4] (Attila node +0xC30 / +0xC34), RIGID_NODE_ENTRY_V2::get_lod,
// then a loop over the level's parts (lod+0x20 count, lod+0x24 list) and a second loop for the previous level while it
// fades. The three known builds hold this function with the same layout (only addresses and one TLS offset differ).
// Different from Attila: the part is in esi (Attila ebx), loop 1 pushes nothing before the part load (Attila: one
// argument), and loop 2 reads the instance parameters from ebx because its body reuses their stack slot [esp+38h].
//
// Rule as Attila's: skip a part whose box is under 1 m across and whose size on screen is under the threshold; head
// parts (box centred over the body at head height) get a gentler threshold. Thresholds are 4K pixels at every
// resolution. Only animated models (instance parameters +0x1C = the pose, as WEIGHTED_MESH_V5::add_instance +0x14DC340
// requires before it makes an item) are touched.
// The part's box at +0x1C (min xyz, max xyz): Attila's probe read it there; Rome 2's mesh base constructor (+0x13D31D0)
// initialises the same 6 floats at +0x1C as Attila's (+0x16DF540), with the same fields around them. Not read in game.
#include <windows.h>
#include <string.h>
#include <math.h>
#include "builds.h"

#define PC_LEN 0x12CAu   // the function's length
#define PC_LOD 0x2A0u    // after the detail-level pick: mov edx,eax ; mov [esp+30h],edx
#define PC_P1 0x430u     // part loop: mov eax,[edx+24h] ; mov esi,[eax+ecx*4]
#define PC_P1_NEXT 0xA24u   // its increment block (reloads ecx and edx)
#define PC_P2 0xA63u     // the fading previous level's loop: mov edx,[esp+74h] ; mov esi,[eax+ecx*4]
#define PC_P2_NEXT 0x113Eu  // its increment block (reloads ecx and esi); the loop's own "skip this part" goes there too
static const unsigned char PC_LOD_B[6] = {0x8B, 0xD0, 0x89, 0x54, 0x24, 0x30};
static const unsigned char PC_P1_B[6] = {0x8B, 0x42, 0x24, 0x8B, 0x34, 0x88};
static const unsigned char PC_P2_B[7] = {0x8B, 0x54, 0x24, 0x74, 0x8B, 0x34, 0x88};
static const unsigned char PC_P1_NEXT_B[8] = {0x8B, 0x4C, 0x24, 0x2C, 0x8B, 0x54, 0x24, 0x30};
static const unsigned char PC_P2_NEXT_B[8] = {0x8B, 0x4C, 0x24, 0x24, 0x8B, 0x74, 0x24, 0x7C};
// per known build (builds.h order): the function, FNV-1a of its code in 0x2F0-byte pieces with relocations undone
static const DWORD g_pc_builds[NBUILDS][2] = {
    {0x14DA460, 0xDAC6105Bu},   // Steam 69CE2041
    {0x14B3F80, 0xA2DE391Au},   // pantheon beta 6AB978BD
    {0x14DA250, 0x7B3BA0B5u},   // GOG 6A564E92
};

// read by the stubs; set once at install
void *pcull_ret_lod, *pcull_ret_p1, *pcull_ret_p2, *pcull_skip_p1, *pcull_skip_p2;

volatile LONG g_partcull_on;              // off until the setting turns it on
float g_partcull_px = 22.0f;              // Attila: the most that showed no pop-in in game
float g_partcull_head_px = 9.0f;
static const float PC_MAX_DIAG = 1.0f;    // longer parts (spears, lances) shape the silhouette: never culled
static const float PC_F = 2607.0f;        // 2160 / (2 tan 22.5 deg): the thresholds are 4K pixels everywhere
// A caller can hand the engine a distance instead of the camera's (instance parameters +0xA8 here, Attila +0xA4). In
// Attila one such distance was 500 for soldiers in front of the camera; a soldier past ~200 m is a flat imposter with no
// parts, so a larger distance is not believed and nothing is culled on it (counted as "forced").
static const float PC_MAX_DIST = 250.0f;
static DWORD pcull_tls = TLS_OUT_OF_INDEXES;

// the part loops may run on several threads at once, so the model's distance and the counters are per thread
struct pcull_th { float dist; unsigned parts, skipped, forced; struct pcull_th *next; };
static struct pcull_th *pcull_threads;
static CRITICAL_SECTION pcull_cs;

static struct pcull_th *pcull_me(void) {
    struct pcull_th *t = TlsGetValue(pcull_tls);
    if (t) return t;
    t = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *t);
    if (!t) return NULL;
    t->dist = -1;
    EnterCriticalSection(&pcull_cs);
    t->next = pcull_threads;
    pcull_threads = t;
    LeaveCriticalSection(&pcull_cs);
    TlsSetValue(pcull_tls, t);
    return t;
}

void partcull_counts(unsigned *parts, unsigned *skipped) {
    *parts = *skipped = 0;
    for (struct pcull_th *t = pcull_threads; t; t = t->next) { *parts += t->parts; *skipped += t->skipped; }
}
// parts the engine placed at a distance of its own choosing, which this fix leaves alone
unsigned partcull_forced(void) {
    unsigned n = 0;
    for (struct pcull_th *t = pcull_threads; t; t = t->next) n += t->forced;
    return n;
}

// the model's distance, from the metric the engine just built: (node[0xA58] * distance)^2 + small terms
void __cdecl pcull_lod(unsigned metric_bits, const unsigned char *node) {
    struct pcull_th *t = pcull_me();
    if (!t) return;
    float m, scale;
    memcpy(&m, &metric_bits, 4);
    memcpy(&scale, node + 0xA58, 4);
    t->dist = (scale > 1e-6f && m > 0) ? sqrtf(m) / scale : -1.0f;
}

// 1 = do not make a draw item for this part
int __cdecl pcull_part(const unsigned char *part, const unsigned *inst) {
    struct pcull_th *t = pcull_me();
    if (!t) return 0;
    t->parts++;
    if (!g_partcull_on || !inst || !inst[7] || t->dist <= 0) return 0;   // inst[7] = the pose: only animated models
    if (t->dist > PC_MAX_DIST) { t->forced++; return 0; }   // not a camera distance: see above
    float b[6];
    memcpy(b, part + 0x1C, sizeof b);
    float dx = b[3] - b[0], dy = b[4] - b[1], dz = b[5] - b[2];
    if (!(dx >= 0 && dy >= 0 && dz >= 0)) return 0;   // not a box, or NaN: keep
    float diag = sqrtf(dx * dx + dy * dy + dz * dz);
    // > 0: the constructor's box is a point; a part still holding it is kept (not in Attila's copy)
    if (!(diag > 0) || diag >= PC_MAX_DIAG) return 0;
    float cx = (b[0] + b[3]) * 0.5f, cy = (b[1] + b[4]) * 0.5f, cz = (b[2] + b[5]) * 0.5f;
    int head = fabsf(cx) < 0.25f && fabsf(cz) < 0.3f && cy > 1.4f && cy < 2.1f;
    float px = head ? g_partcull_head_px : g_partcull_px;
    if (diag * PC_F >= px * t->dist) return 0;
    t->skipped++;
    return 1;
}

// Each stub replays the instructions its site held, then either returns to the game or jumps to that loop's increment
// block. Every register and flag is preserved; the increment blocks reload what they need themselves. XMM registers are
// not saved: all three sites follow a call on every path into them (MSVC x86: XMM is volatile across calls).
// Stack offsets below: the game's [esp+X] is X + 36 after pushal + pushfl, + 4 per argument already pushed.
__attribute__((naked)) static void pcull_stub_lod(void) {
    __asm__ volatile(
        "pushal\n pushfl\n"
        "pushl 0x64(%esp)\n pushl 0x44(%esp)\n"   // the node ([esp+40h]), the metric ([esp+1Ch])
        "call _pcull_lod\n addl $8, %esp\n"
        "popfl\n popal\n"
        "movl %eax, %edx\n movl %edx, 0x30(%esp)\n"
        "jmp *_pcull_ret_lod\n");
}
__attribute__((naked)) static void pcull_stub_p1(void) {
    __asm__ volatile(
        "movl 0x24(%edx), %eax\n movl (%eax,%ecx,4), %esi\n"
        "pushal\n pushfl\n"
        "pushl 0x5C(%esp)\n pushl %esi\n"         // the instance parameters ([esp+38h]), the part
        "call _pcull_part\n addl $8, %esp\n"
        "testl %eax, %eax\n"
        "jnz 1f\n"
        "popfl\n popal\n jmp *_pcull_ret_p1\n"
        "1: popfl\n popal\n jmp *_pcull_skip_p1\n");
}
__attribute__((naked)) static void pcull_stub_p2(void) {
    __asm__ volatile(
        "movl 0x74(%esp), %edx\n movl (%eax,%ecx,4), %esi\n"
        "pushal\n pushfl\n"
        "pushl %ebx\n pushl %esi\n"               // the instance parameters (ebx in this loop), the part
        "call _pcull_part\n addl $8, %esp\n"
        "testl %eax, %eax\n"
        "jnz 1f\n"
        "popfl\n popal\n jmp *_pcull_ret_p2\n"
        "1: popfl\n popal\n jmp *_pcull_skip_p2\n");
}

// FNV-1a over the per-piece hashes of the function, relocations undone (fnv1a_unreloc takes at most 0x2F8 bytes)
static DWORD pc_hash(const unsigned char *b, DWORD rva) {
    DWORD h = 0x811C9DC5u;
    for (DWORD o = 0; o < PC_LEN; o += 0x2F0) h = (h ^ fnv1a_unreloc(b, rva + o, PC_LEN - o < 0x2F0 ? PC_LEN - o : 0x2F0)) * 0x01000193u;
    return h;
}

// jmp rel32 at `site` to `to`, NOPs up to len (from Attila's patch.h)
static int pc_jump(unsigned char *site, unsigned len, const void *to) {
    DWORD old, rel = (DWORD)((const unsigned char *)to - (site + 5));
    if (!VirtualProtect(site, len, PAGE_EXECUTE_READWRITE, &old)) return 0;
    site[0] = 0xE9;
    memcpy(site + 1, &rel, 4);
    memset(site + 5, 0x90, len - 5);
    VirtualProtect(site, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, len);
    return 1;
}

// 1 installed, 0 not (another game build, or already installed). Patches code: call before the game's threads run it.
int install_partcull(HMODULE m, void (*log)(const char *fmt, ...)) {
    static int done;
    if (done) return 0;
    unsigned char *b = (unsigned char *)m, *f = NULL;
    for (int i = 0; i < NBUILDS && !f; i++)
        if (pc_hash(b, g_pc_builds[i][0]) == g_pc_builds[i][1]) f = b + g_pc_builds[i][0];
    if (!f || memcmp(f + PC_LOD, PC_LOD_B, 6) || memcmp(f + PC_P1, PC_P1_B, 6) || memcmp(f + PC_P2, PC_P2_B, 7) ||
        memcmp(f + PC_P1_NEXT, PC_P1_NEXT_B, 8) || memcmp(f + PC_P2_NEXT, PC_P2_NEXT_B, 8)) {
        log("unit detail cull: the game's code differs from the known game versions - not installed");
        return 0;
    }
    if (pcull_tls == TLS_OUT_OF_INDEXES && (pcull_tls = TlsAlloc()) == TLS_OUT_OF_INDEXES) return 0;
    InitializeCriticalSection(&pcull_cs);
    pcull_ret_lod = f + PC_LOD + 6;
    pcull_ret_p1 = f + PC_P1 + 6;
    pcull_ret_p2 = f + PC_P2 + 7;
    pcull_skip_p1 = f + PC_P1_NEXT;
    pcull_skip_p2 = f + PC_P2_NEXT;
    // the loops first: a part stub with no distance yet culls nothing; the detail-level stub last
    if (!pc_jump(f + PC_P1, 6, (void *)pcull_stub_p1) || !pc_jump(f + PC_P2, 7, (void *)pcull_stub_p2) ||
        !pc_jump(f + PC_LOD, 6, (void *)pcull_stub_lod)) {
        // shortcut: a site already patched stays patched (its stub culls nothing without the detail-level stub's
        // distance), fine while VirtualProtect on the game's own code never fails; undo the sites if it ever does
        log("unit detail cull: VirtualProtect failed (%lu) - not installed", GetLastError());
        return 0;
    }
    done = 1;
    log("unit detail cull installed: when on, soldier parts under %.0f px tall (helmets and heads %.0f px) are not drawn",
        g_partcull_px, g_partcull_head_px);
    return 1;
}
