// Sound crash guard (from Attila 7.1, Attila AGENT_LOG 225): the game's sound engine (Wwise, inside empire.retail.dll)
// gives each mixer an output buffer from its fixed lower-engine pool. When that pool is full, the mixer's setup leaves
// the buffer pointer and frame count at 0 but keeps the old channel count, and the voice is still mixed: the gain-ramp
// mix (cdecl(src, dst, gain0, gain1, frames)) then writes to address 0 and the game closes (Attila: huge MK1212 battles,
// once leaving a battle). Rome 2 has the same mix function, instruction for instruction (only its addresses differ,
// capstone 2026-11-23), with the same 5 call sites at the same distances, in all three known builds; the pool-full
// chain itself was traced in Attila's code only. Here the 5 call sites go through mg_mix, which skips a mix whose source
// or destination is a null pointer (plus a small offset): that voice is silent for one buffer instead.
#include <windows.h>
#include <string.h>
#include "builds.h"

#define LEN_MIX 0x124u
// per known build (builds.h order): the mix function, FNV-1a of its code with relocations undone, its 5 call sites
static const DWORD g_sg_builds[NBUILDS][7] = {
    {0x1698E90, 0x40CEE97Eu, 0x168B7B5, 0x168B848, 0x1692533, 0x169260A, 0x16926A6},   // Steam 69CE2041
    {0x16724B0, 0x3BCC2551u, 0x1664DE5, 0x1664E78, 0x166BB53, 0x166BC2A, 0x166BCC6},   // pantheon beta 6AB978BD
    {0x1698C80, 0x8BBDCB12u, 0x168B5B5, 0x168B648, 0x1692323, 0x16923FA, 0x1692496},   // GOG 6A564E92
};
#define NSITES 5
#define LOW 0x10000u   // no buffer lives in the first 64 KB: Windows never maps it

typedef void(__cdecl *mix_fn)(float *src, float *dst, float gain0, float gain1, unsigned frames);
static mix_fn g_mix;
static const DWORD *g_sites;       // the build's entry: [2..6] the call sites
volatile LONG g_soundguard_skips;  // the first skip is logged once
static void (*g_sg_log)(const char *fmt, ...);

static void __cdecl mg_mix(float *src, float *dst, float gain0, float gain1, unsigned frames) {
    if ((UINT_PTR)src < LOW || (UINT_PTR)dst < LOW) {
        if (InterlockedIncrement(&g_soundguard_skips) == 1 && g_sg_log)
            g_sg_log("sound: a sound had no buffer (the game's sound memory pool is full) - that sound went silent for a moment instead of crashing the game");
        return;
    }
    g_mix(src, dst, gain0, gain1, frames);
}

// the entry whose function and call sites match m, or NULL
static const DWORD *sg_find(unsigned char *b) {
    for (int i = 0; i < NBUILDS; i++) {
        const DWORD *e = g_sg_builds[i];
        if (fnv1a_unreloc(b, e[0], LEN_MIX) != e[1]) continue;
        int k = 0;
        for (; k < NSITES; k++) {
            unsigned char *s = b + e[2 + k];
            if (!in_image(b, e[2 + k], 5) || s[0] != 0xE8 || s + 5 + *(LONG *)(s + 1) != b + e[0]) break;
        }
        if (k == NSITES) return e;
    }
    return NULL;
}

int install_soundguard(HMODULE m, void (*log)(const char *fmt, ...)) {
    unsigned char *b = (unsigned char *)m;
    const DWORD *e = sg_find(b);
    if (!e) {
        log("sound crash guard: the game's sound mixing code differs from the known game versions - not installed");
        return 0;
    }
    g_sites = e;
    g_mix = (mix_fn)(b + e[0]);
    g_sg_log = log;
    for (int i = 0; i < NSITES; i++) {
        unsigned char *s = b + e[2 + i];
        DWORD old;
        if (!VirtualProtect(s, 5, PAGE_EXECUTE_READWRITE, &old)) {
            log("sound crash guard: VirtualProtect failed (%lu) - %d of %d call sites guarded", GetLastError(), i, NSITES);
            return 0;  // shortcut: sites already retargeted stay guarded; mg_mix forwards to the same function
        }
        LONG rel = (LONG)((unsigned char *)mg_mix - (s + 5));
        memcpy(s + 1, &rel, 4);  // the call stays a call, to mg_mix
        VirtualProtect(s, 5, old, &old);
    }
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
    log("sound crash guard: a sound without a buffer goes silent for a moment instead of crashing the game");
    return 1;
}
