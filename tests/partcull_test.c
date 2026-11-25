// Tests partcull.c (from Attila's partcull_test.c):
//   1. the rule (pcull_lod sets a model's distance, pcull_part answers for each part), on made-up parts;
//   2. the three stubs, run on a made-up stack frame laid out as the game's: each replays the instructions it replaced,
//      reads the metric / node / part / instance parameters from where the game keeps them at that site, keeps every
//      other register and esp, and goes on to the game (kept) or to the loop's increment block (skipped);
//   3. the install on the game's real code (argument: empire.retail.dll, mapped without running it; each known build):
//      refused on a module that is not the game and on the game with one byte changed, then all three sites patched
//      into this module with the bytes after them left alone, and a second install refused.
// usage: partcull_test.exe <path to empire.retail.dll>
#include "../partcull.c"
#include <stdio.h>
#include <stdarg.h>

static int g_fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_fails++; } } while (0)

static unsigned char g_node[0xA60], g_part[0x100], g_spear[0x100];
static unsigned g_inst[64];
static void dist(float metres, float scale) {   // as the engine's metric: (scale * distance)^2
    float m = scale * metres * scale * metres;
    unsigned bits;
    memcpy(g_node + 0xA58, &scale, 4);
    memcpy(&bits, &m, 4);
    pcull_lod(bits, g_node);
}
static void boxp(unsigned char *p, float x0, float y0, float z0, float x1, float y1, float z1) {
    float b[6] = {x0, y0, z0, x1, y1, z1};
    memcpy(p + 0x1C, b, sizeof b);
}
#define box(...) boxp(g_part, __VA_ARGS__)
static int part(void) { return pcull_part(g_part, g_inst); }

static volatile int g_other = -1;
static DWORD WINAPI other_thread(void *a) {   // a thread that was never given a distance
    (void)a;
    g_other = part();
    return 0;
}
static void quiet(const char *fmt, ...) { (void)fmt; }
static char g_said[512];
static void say(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vsnprintf(g_said, sizeof g_said, fmt, a);
    va_end(a);
}

// ---- the stub harness: registers in, a jump to the stub with esp = g_frame, registers and esp out ----
unsigned pc_in[7], pc_out[7], pc_esp_out, pc_where;   // eax ecx edx ebx esi edi ebp; 1 kept, 2 skipped
void *pc_stub, *pc_saved_esp;
unsigned char *pc_frame;
extern char pc_keep[], pc_skip[];
void pc_run(void);
__asm__(".text\n.globl _pc_run\n_pc_run:\n"
        " pushal\n movl %esp, _pc_saved_esp\n movl _pc_frame, %esp\n"
        " movl _pc_in, %eax\n movl _pc_in+4, %ecx\n movl _pc_in+8, %edx\n movl _pc_in+12, %ebx\n"
        " movl _pc_in+16, %esi\n movl _pc_in+20, %edi\n movl _pc_in+24, %ebp\n"
        " jmp *_pc_stub\n"
        ".globl _pc_keep\n_pc_keep:\n movl $1, _pc_where\n jmp 2f\n"
        ".globl _pc_skip\n_pc_skip:\n movl $2, _pc_where\n"
        "2:\n movl %eax, _pc_out\n movl %ecx, _pc_out+4\n movl %edx, _pc_out+8\n movl %ebx, _pc_out+12\n"
        " movl %esi, _pc_out+16\n movl %edi, _pc_out+20\n movl %ebp, _pc_out+24\n movl %esp, _pc_esp_out\n"
        " movl _pc_saved_esp, %esp\n popal\n ret\n");
static unsigned char g_stack[0x800] __attribute__((aligned(16)));
static void run(void (*stub)(void)) {
    pc_stub = (void *)stub;
    pc_where = 0;
    pc_run();
}
static void slot(unsigned off, unsigned v) { memcpy(pc_frame + off, &v, 4); }
static unsigned get(unsigned off) { unsigned v; memcpy(&v, pc_frame + off, 4); return v; }
// registers the stub must not change, against what went in
static int same(int skip_mask) {
    for (int i = 0; i < 7; i++)
        if (!(skip_mask >> i & 1) && pc_out[i] != pc_in[i]) return 0;
    return pc_esp_out == (unsigned)(UINT_PTR)pc_frame;
}
enum { EAX = 1, ECX = 2, EDX = 4, EBX = 8, ESI = 16 };

static void stubs(void) {
    pc_frame = g_stack + 0x400;
    pcull_ret_lod = pcull_ret_p1 = pcull_ret_p2 = pc_keep;
    pcull_skip_p1 = pcull_skip_p2 = pc_skip;
    unsigned regs[7] = {0x11110000, 0x22220000, 0x33330000, 0x44440000, 0x55550000, 0x66660000, 0x77770000};
    // detail-level pick: eax = the level get_lod chose; [esp+1Ch] the metric, [esp+40h] the node
    float one = 1.0f, metric = 100.0f * 100.0f;
    unsigned mb;
    memcpy(g_node + 0xA58, &one, 4);
    memcpy(&mb, &metric, 4);
    memset(g_stack, 0xEE, sizeof g_stack);
    slot(0x1C, mb);
    slot(0x40, (unsigned)(UINT_PTR)g_node);
    memcpy(pc_in, regs, sizeof regs);
    pc_in[0] = 0x0BADF00D;
    pcull_me()->dist = -1;
    run(pcull_stub_lod);
    CHECK(pc_where == 1 && same(EDX) && pc_out[2] == 0x0BADF00D && get(0x30) == 0x0BADF00D);   // mov edx,eax ; mov [esp+30h],edx
    CHECK(fabsf(pcull_me()->dist - 100.0f) < 0.01f);
    // loop 1: edx = the level (+0x24 its parts), ecx = the index, [esp+38h] the instance parameters
    const unsigned char *parts[3] = {g_spear, g_part, g_spear};
    unsigned char lod[0x30] = {0};
    const unsigned char **pp = parts;
    memcpy(lod + 0x24, &pp, 4);
    boxp(g_part, -0.15f, 0.9f, -0.05f, 0.15f, 1.0f, 0.05f);    // a belt: 8 px at 100 m
    boxp(g_spear, -0.02f, 0.0f, -0.02f, 0.02f, 2.4f, 0.02f);   // a spear: never culled
    slot(0x38, (unsigned)(UINT_PTR)g_inst);
    memcpy(pc_in, regs, sizeof regs);
    pc_in[1] = 1;
    pc_in[2] = (unsigned)(UINT_PTR)lod;
    pc_in[3] = 0;   // ebx is not the parameters in this loop
    run(pcull_stub_p1);
    CHECK(pc_where == 2 && same(EAX | ESI) && pc_out[0] == (unsigned)(UINT_PTR)parts && pc_out[4] == (unsigned)(UINT_PTR)g_part);
    pc_in[1] = 2;
    run(pcull_stub_p1);
    CHECK(pc_where == 1 && same(EAX | ESI) && pc_out[4] == (unsigned)(UINT_PTR)g_spear);
    // loop 2: eax = the previous level's parts, ecx = the index, ebx the instance parameters ([esp+38h] is reused there)
    slot(0x38, 0);
    slot(0x74, 0x0CAFE);
    memcpy(pc_in, regs, sizeof regs);
    pc_in[0] = (unsigned)(UINT_PTR)parts;
    pc_in[1] = 1;
    pc_in[3] = (unsigned)(UINT_PTR)g_inst;
    run(pcull_stub_p2);
    CHECK(pc_where == 2 && same(EDX | ESI) && pc_out[2] == 0x0CAFE && pc_out[4] == (unsigned)(UINT_PTR)g_part);
    pc_in[1] = 0;
    run(pcull_stub_p2);
    CHECK(pc_where == 1 && same(EDX | ESI) && pc_out[4] == (unsigned)(UINT_PTR)g_spear);
    if (!g_fails) printf("stubs: replayed instructions, registers, esp and both exits as expected\n");
}

int main(int argc, char **argv) {
    // what install_partcull sets up besides the patches
    pcull_tls = TlsAlloc();
    InitializeCriticalSection(&pcull_cs);
    InterlockedExchange(&g_partcull_on, 1);
    g_partcull_px = 22.0f; g_partcull_head_px = 9.0f;
    g_inst[7] = 1;                       // a pose: an animated model
    // a belt: 0.3 m across at hip height. On screen: 0.3 * 2607 / distance pixels
    box(-0.15f, 0.9f, -0.05f, 0.15f, 1.0f, 0.05f);
    dist(100.0f, 1.0f); CHECK(part() == 1);            // 8.2 px < 22: not drawn
    dist(100.0f, 0.37f); CHECK(part() == 1);           // the node's scale drops out of the distance
    dist(30.0f, 1.0f); CHECK(part() == 0);             // 27 px: drawn
    dist(40.0f, 1.0f); CHECK(part() == 1);             // 20.6 px: just under
    g_partcull_px = 16.0f;
    CHECK(part() == 0);                                // the panel's lighter setting keeps it
    g_partcull_px = 22.0f;
    // a spear: longer than 1 m, shapes the silhouette, never culled
    box(-0.02f, 0.0f, -0.02f, 0.02f, 2.4f, 0.02f);
    dist(200.0f, 1.0f); CHECK(part() == 0);
    // a helmet: centred over the body at head height, the gentler threshold of its own
    box(-0.12f, 1.6f, -0.14f, 0.12f, 1.85f, 0.14f);    // 0.42 m across
    dist(60.0f, 1.0f); CHECK(part() == 0);             // 18 px: a belt would go, the helmet stays
    dist(150.0f, 1.0f); CHECK(part() == 1);            // 7.3 px < 9
    // the same box off to the side is a shoulder piece, not a head: the body threshold
    box(0.3f, 1.6f, -0.14f, 0.54f, 1.85f, 0.14f);
    dist(60.0f, 1.0f); CHECK(part() == 1);
    // what must never be culled
    box(-0.15f, 0.9f, -0.05f, 0.15f, 1.0f, 0.05f);
    dist(100.0f, 1.0f);
    g_inst[7] = 0; CHECK(part() == 0); g_inst[7] = 1;                 // not animated: a prop, a building, a tree
    CHECK(pcull_part(g_part, NULL) == 0);                             // no instance parameters
    InterlockedExchange(&g_partcull_on, 0); CHECK(part() == 0); InterlockedExchange(&g_partcull_on, 1);   // switched off
    dist(500.0f, 1.0f); CHECK(part() == 0);                           // a forced distance: not believed
    CHECK(partcull_forced() == 1);
    dist(0.0f, 1.0f); CHECK(part() == 0);                             // the engine gave no distance
    {   unsigned bits; float m = 10000.0f, z = 0.0f; memcpy(g_node + 0xA58, &z, 4); memcpy(&bits, &m, 4); pcull_lod(bits, g_node); }
    CHECK(part() == 0);                                               // a zero scale: no distance to be had
    dist(100.0f, 1.0f);
    box(0.15f, 0.9f, -0.05f, -0.15f, 1.0f, 0.05f); CHECK(part() == 0);    // not a box (min above max)
    {   float n = 0.0f; n = n / n; box(n, 0.9f, -0.05f, 0.15f, 1.0f, 0.05f); CHECK(part() == 0); }   // NaN
    box(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f); CHECK(part() == 0);      // the constructor's point box: never filled
    box(-0.15f, 0.9f, -0.05f, 0.15f, 1.0f, 0.05f);
    CHECK(part() == 1);                                               // and the plain case again
    // the distance belongs to the thread that set it: the part loops may run on several at once
    HANDLE t = CreateThread(NULL, 0, other_thread, NULL, 0, NULL);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    CHECK(g_other == 0);
    unsigned parts, skipped;
    partcull_counts(&parts, &skipped);
    CHECK(parts == 20 && skipped == 6);   // 19 asked here and 1 on the other thread; the six "== 1" above
    printf("rule: %u parts asked, %u not drawn, %u at a forced distance\n", parts, skipped, partcull_forced());

    stubs();

    if (argc > 1) {
        // its preferred base taken, so the loader moves it as in game and the byte checks must undo the relocations
        CHECK(VirtualAlloc((void *)0x10000000, 0x100000, MEM_RESERVE, PAGE_NOACCESS) != NULL);
        HMODULE m = LoadLibraryExA(argv[1], NULL, DONT_RESOLVE_DLL_REFERENCES);
        CHECK(m != NULL && m != (HMODULE)0x10000000);
        if (m) {
            unsigned char *b = (unsigned char *)m;
            const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(b + ((const IMAGE_DOS_HEADER *)b)->e_lfanew);
            const char *name = build_name(nt->FileHeader.TimeDateStamp);
            DeleteCriticalSection(&pcull_cs);
            pcull_tls = TLS_OUT_OF_INDEXES;   // as at a real start
            HMODULE self = GetModuleHandleW(NULL);
            CHECK(install_partcull(self, say) == 0 && strstr(g_said, "not installed"));   // not the game
            unsigned char *f = NULL;
            for (int i = 0; i < NBUILDS; i++)
                if (pc_hash(b, g_pc_builds[i][0]) == g_pc_builds[i][1]) f = b + g_pc_builds[i][0];
            CHECK(f != NULL);
            if (f) {
                // one byte of the function changed (a game update): refused, nothing written
                DWORD old;
                unsigned char keep = f[0x100];
                CHECK(VirtualProtect(f + 0x100, 1, PAGE_EXECUTE_READWRITE, &old));
                f[0x100] ^= 1;
                unsigned char lod0 = f[PC_LOD];
                CHECK(install_partcull(m, say) == 0 && strstr(g_said, "not installed") && f[PC_LOD] == lod0);
                f[0x100] = keep;
                VirtualProtect(f + 0x100, 1, old, &old);
                static const unsigned site[3] = {PC_LOD, PC_P1, PC_P2}, len[3] = {6, 6, 7};
                unsigned char before[3][16];
                for (int i = 0; i < 3; i++) memcpy(before[i], f + site[i], 16);
                int r = install_partcull(m, say);
                printf("install on %s: %d - %s\n", name ? name : "an unknown build", r, g_said);
                unsigned char *lo = (unsigned char *)self;
                unsigned char *hi = lo + ((const IMAGE_NT_HEADERS *)(lo + ((const IMAGE_DOS_HEADER *)lo)->e_lfanew))->OptionalHeader.SizeOfImage;
                CHECK(r == 1 && strstr(g_said, "unit detail cull installed"));
                for (int i = 0; i < 3; i++) {
                    unsigned char *s = f + site[i];
                    int rel;
                    memcpy(&rel, s + 1, 4);
                    unsigned char *to = s + 5 + rel;
                    CHECK(s[0] == 0xE9 && to >= lo && to < hi);                       // a jump into this module
                    for (unsigned k = 5; k < len[i]; k++) CHECK(s[k] == 0x90);         // the rest of the replaced instructions
                    CHECK(!memcmp(before[i] + len[i], s + len[i], 16 - len[i]));       // and nothing past them
                }
                CHECK(pcull_ret_lod == f + PC_LOD + 6 && pcull_ret_p1 == f + PC_P1 + 6 && pcull_ret_p2 == f + PC_P2 + 7);
                CHECK(pcull_skip_p1 == f + PC_P1_NEXT && pcull_skip_p2 == f + PC_P2_NEXT);
                CHECK(install_partcull(m, quiet) == 0);                               // a second install changes nothing
            }
        }
    } else printf("no game DLL given: the install was not tested\n");
    printf(g_fails ? "partcull test: %d FAILED\n" : "all partcull tests passed\n", g_fails);
    return g_fails != 0;
}
