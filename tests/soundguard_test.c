// Tests soundguard.c on a real empire.retail.dll (mapped, not run; each known build): the mix function and its 5 call
// sites are found and retargeted, a second install is refused, a null or near-null source / destination is skipped
// (counted, no crash), and real buffers are mixed exactly as the game's own mix function mixes them (constant and
// ramped gain). build.sh also builds it from a broken copy (no null check) and expects a failure (from Attila's test).
// usage: soundguard_test.exe <path to empire.retail.dll>
#include "../soundguard.c"
#include <stdio.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)
static void nolog(const char *fmt, ...) { (void)fmt; }

int main(int argc, char **argv) {
    CHECK(argc == 2);
    wchar_t w[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, argv[1], -1, w, MAX_PATH);
    unsigned char *b = (unsigned char *)LoadLibraryExW(w, NULL, DONT_RESOLVE_DLL_REFERENCES);
    CHECK(b != NULL);
    CHECK(install_soundguard((HMODULE)b, nolog) == 1);
    for (int i = 0; i < NSITES; i++) {
        unsigned char *s = b + g_sites[2 + i];
        CHECK(s[0] == 0xE8 && s + 5 + *(LONG *)(s + 1) == (unsigned char *)mg_mix);
    }
    CHECK(install_soundguard((HMODULE)b, nolog) == 0);  // sites no longer call the game's mix

    static float src[1024] __attribute__((aligned(16))), dst[1024] __attribute__((aligned(16))),
        ref[1024] __attribute__((aligned(16)));
    for (int i = 0; i < 1024; i++) src[i] = (float)((i * 37) % 101) / 50.0f - 1.0f;

    // the crash: destination 0 (and 0 + a small channel offset), source 0
    mg_mix(src, NULL, 1.0f, 0.0f, 1024);
    mg_mix(src, (float *)0x1000, 0.5f, 0.25f, 1024);
    mg_mix(NULL, dst, 0.5f, 0.0f, 1024);
    CHECK(g_soundguard_skips == 3);

    // real buffers: identical to the game's mix (constant gain, then a ramp), and something was mixed
    float gains[][2] = {{0.7f, 0.0f}, {0.2f, 0.5f / 1024}};
    for (int g = 0; g < 2; g++) {
        for (int i = 0; i < 1024; i++) dst[i] = ref[i] = (float)(i % 7) * 0.1f;
        mg_mix(src, dst, gains[g][0], gains[g][1], 1024);
        g_mix(src, ref, gains[g][0], gains[g][1], 1024);
        CHECK(!memcmp(dst, ref, sizeof dst));
        int changed = 0;
        for (int i = 0; i < 1024; i++) changed += dst[i] != (float)(i % 7) * 0.1f;
        CHECK(changed > 900);
    }
    CHECK(g_soundguard_skips == 3);
    printf("soundguard: OK (build at +0x%lX, 5 sites, null buffers skipped, real mixes identical to the game's)\n", g_sites[0]);
    return 0;
}
