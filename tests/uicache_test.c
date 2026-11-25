// uicache.c (from Attila 7.1's uicache_test.c), two ways:
//   uicache_test.exe <empire.retail.dll>   the real game DLL, mapped and not run (each known build): its table entry is
//       found, Draw and the batch flush get their jumps, the entry stub carries Draw's first instruction as it is in
//       memory and jumps back past it, the clip rect and the renderer global are read from the hashed code, and the
//       patched DLL no longer matches (a second install would be refused).
//   uicache_test.exe [plain]               a real D3D11 device, with a stand-in for the game's component draw that starts
//       with the same instruction (mov eax, [abs]), so the real stubs and the real patches on d3d11 are what runs. A panel
//       of blended quads (over, additive, multiply, each with a viewport of its own, and a render to another texture in
//       the middle) must look the same drawn into the cache and shown from it as drawn straight onto the target - over
//       two different backgrounds, the second without the panel being drawn again - with the cache's textures only as
//       large as the panel. The UI's size comes from the clip rect at the root's Draw, not at the panel's (a clipping
//       ancestor narrows it there). Also: other mods' hooks already on d3d11's Draw (a hot patch) and RSSetViewports (a
//       5-byte jump) keep seeing every call; the DLSS add-on's hooks in the context's vtable (unless "plain") see the
//       game's calls only; an uncached component's draw is entered with the stack and the arguments as its caller set
//       them; the redraw rules (a held button counts when it goes down and comes up only); the state the game finds
//       afterwards; a blend the cache cannot stand in for; and what the batch flush at the panel's edges is for.
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>

volatile LONG g_frame_no;   // fpslog.c in the real build

#ifndef UIC_SRC
#define UIC_SRC "../uicache.c"
#endif
#include UIC_SRC

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s line %d\n", #c, __LINE__); fails++; } } while (0)

static void tlog(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vprintf(fmt, a);
    va_end(a);
    putchar('\n');
}

// ---- the real game DLL
static int real_dll(const char *path) {
    wchar_t w[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, path, -1, w, MAX_PATH);
    unsigned char *b = (unsigned char *)LoadLibraryExW(w, NULL, DONT_RESOLVE_DLL_REFERENCES);
    CHECK(b != NULL);
    if (!b) return 1;
    const DWORD *e = uc_find(b);
    CHECK(e != NULL);
    if (!e) return 1;
    unsigned char *f = b + e[B_DRAW], *fl = b + e[B_FLUSH], first[5];
    memcpy(first, f, 5);
    CHECK(first[0] == 0xA1 && f[5] == 0x81 && f[6] == 0xEC);   // mov eax, [abs]; sub esp, ...
    CHECK(install_uicache((HMODULE)b, NULL, NULL, tlog) == 1 && g_nt == 1 && !strcmp(g_t[0].name, "card_panel_docker"));
    CHECK(f[0] == 0xE9 && f + 5 + *(LONG *)(f + 1) == g_stubs);
    CHECK(!memcmp(g_stubs + 0x14, first, 5) && g_stubs + 0x1E + *(LONG *)(g_stubs + 0x1A) == f + 5);   // the moved instruction, then back past it
    CHECK(fl[0] == 0xE9 && fl + 5 + *(LONG *)(fl + 1) == g_stubs + 0x40 && fl[5] == 0x90);
    CHECK(g_stubs[0x4C] == 0xE9 && g_stubs + 0x51 + *(LONG *)(g_stubs + 0x4D) == fl + 6 && !memcmp(g_stubs + 0x46, "\x83\xEC\x4C\x56\x8B\xF1", 6));
    // the clip rect and the renderer global, from the code: inside the DLL's data, the clip rect where the root writes it
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(b + ((const IMAGE_DOS_HEADER *)b)->e_lfanew);
    DWORD size = nt->OptionalHeader.SizeOfImage;
    CHECK((UINT_PTR)g_clip - (UINT_PTR)b < size && (UINT_PTR)g_renderer - (UINT_PTR)b < size && g_root_wh == g_ui_wh);
    CHECK(*(const float *const *)(b + e[B_DRAW] + e[B_CLIP_AT]) == g_clip && b[e[B_DRAW] + e[B_CLIP_AT] - 2] == 0xC7);
    CHECK(*g_renderer == NULL && device() == NULL);   // not run: no renderer, nothing cached
    CHECK(uc_find(b) == NULL);   // patched: no longer the known code
    int build = (int)(e - g_uc_builds[0]) / 10;
    if (!fails) printf("uicache %s: OK (build %d, Draw +0x%lX, flush +0x%lX, clip rect +0x%lX, renderer +0x%lX)\n", path, build, e[B_DRAW], e[B_FLUSH],
                       (unsigned long)((const unsigned char *)g_clip - b), (unsigned long)((unsigned char *)g_renderer - b));
    return fails ? 1 : 0;
}

// ---- a real device
static const char *test_hlsl =
    "float4 vs(uint i : SV_VertexID) : SV_Position { float2 t = float2((i << 1) & 2, i & 2); return float4(t * float2(2, -2) + float2(-1, 1), 0.5, 1); }\n"
    "cbuffer k { float4 col; }; float4 ps() : SV_Target { return col; }\n";

enum { W = 256 };
static ID3D11Device *dev;
static ID3D11DeviceContext *c;
static ID3D11Texture2D *bbt;
static ID3D11RenderTargetView *bb, *side;
static ID3D11VertexShader *tvs;
static ID3D11PixelShader *tps;
static ID3D11Buffer *cb;
static ID3D11BlendState *over, *add, *mul, *dsta;
static int ndraws, last_blend;   // last_blend: 0 additive, 1 multiply, 2 one that reads the target's alpha
static void *seen_ret, *seen_self, *seen_details;
static unsigned seen_a;
static char seen_b, seen_c;
static volatile LONG fw_calls, mh_calls;   // the other mods' detours on Draw and RSSetViewports
static float clip[4];                      // the game's clip rect: the root sets it to the UI's size, a clipping ancestor narrows it
static DWORD first_global;                 // what the stand-in's first instruction reads
static float state72[16];                  // a component state: width at +0x28, height at +0x2C

// The DLSS add-on's hooks, as bridgeprobe.c has them: in the context's vtable, on OMSetRenderTargets, RSSetViewports
// and RSGetViewports; it remembers the viewport the game set and answers the game's reads with it. It must see the
// game's calls only: never the cache's textures as a target, never a viewport the game did not set.
static void(STDMETHODCALLTYPE *ad_rt)(ID3D11DeviceContext *, UINT, ID3D11RenderTargetView *const *, ID3D11DepthStencilView *);
static void(STDMETHODCALLTYPE *ad_vp)(ID3D11DeviceContext *, UINT, const D3D11_VIEWPORT *);
static D3D11_VIEWPORT ad_gvp, game_vps[32];
static int ad_rt_calls, ad_vp_calls, ad_foreign_rt, ad_foreign_vp, n_game_vps;
static void game_vp(const D3D11_VIEWPORT *v) {   // the game sets a viewport: the test remembers each one it uses
    int i = 0;
    while (i < n_game_vps && memcmp(&game_vps[i], v, sizeof *v)) i++;
    if (i == n_game_vps && n_game_vps < 32) game_vps[n_game_vps++] = *v;
    ID3D11DeviceContext_RSSetViewports(c, 1, v);
}
static void STDMETHODCALLTYPE aw_rt(ID3D11DeviceContext *cc, UINT n, ID3D11RenderTargetView *const *rtv, ID3D11DepthStencilView *dsv) {
    ad_rt_calls++;
    if (n && rtv[0] != bb && rtv[0] != side) ad_foreign_rt++;
    ad_rt(cc, n, rtv, dsv);
}
static void STDMETHODCALLTYPE aw_vp(ID3D11DeviceContext *cc, UINT n, const D3D11_VIEWPORT *v) {
    int known = 0;
    for (int i = 0; i < n_game_vps; i++) known |= n && !memcmp(&game_vps[i], v, sizeof *v);
    ad_vp_calls++;
    ad_foreign_vp += !known;
    if (n) ad_gvp = v[0];
    ad_vp(cc, n, v);
}
static void STDMETHODCALLTYPE aw_getvp(ID3D11DeviceContext *cc, UINT *n, D3D11_VIEWPORT *v) {
    (void)cc;
    if (v && *n) v[0] = ad_gvp;
    *n = 1;
}
// what bridgeprobe.c is to give the cache: d3d11's own function behind the add-on's hook in a slot, NULL for others
void *__cdecl fgb_d3d11_original(unsigned slot) {
    void *fn = (*(void ***)c)[slot];
    return fn == (void *)aw_rt ? (void *)ad_rt : fn == (void *)aw_vp ? (void *)ad_vp : NULL;
}

static void quad(float x, float y, float w, float h, float r, float g, float b, float a, ID3D11BlendState *bs) {
    static const FLOAT one[4] = {1, 1, 1, 1};
    D3D11_VIEWPORT vp = {x, y, w, h, 0, 1};
    D3D11_MAPPED_SUBRESOURCE m;
    FLOAT col[4] = {r, g, b, a};
    game_vp(&vp);
    if (SUCCEEDED(ID3D11DeviceContext_Map(c, (ID3D11Resource *)cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, col, sizeof col);
        ID3D11DeviceContext_Unmap(c, (ID3D11Resource *)cb, 0);
    }
    if (bs) ID3D11DeviceContext_OMSetBlendState(c, bs, one, 0xFFFFFFFF);
    ID3D11DeviceContext_Draw(c, 3, 0);
}

// The game's batching: a quad is not drawn when its component asks for it but when the next one with another state
// comes, or at the frame's end. batch_flush is what g_flush stands for; it notes the batch, as the stub at the start of
// the game's flush does.
static struct { float v[8]; ID3D11BlendState *bs; int on; } pend;
static void __fastcall batch_flush(void *b, void *edx) {
    (void)edx;
    g_uic_batch = b;
    if (!pend.on) return;
    pend.on = 0;
    quad(pend.v[0], pend.v[1], pend.v[2], pend.v[3], pend.v[4], pend.v[5], pend.v[6], pend.v[7], pend.bs);
}
static void later(float x, float y, float w, float h, float r, float g, float b, float a, ID3D11BlendState *bs) {
    batch_flush(&pend, NULL);
    pend = (__typeof__(pend)){{x, y, w, h, r, g, b, a}, bs, 1};
}

static unsigned char comp[0x200], comp2[0x200], plain[0x200], hud[0x200], det[0x40];
// the game's component draw. "plain" is the one before the panel (a root here, far outside the panel): its quad is still
// waiting when the panel begins. The panel, in the middle of the target: its first quad relies on the blend state bound
// before it, after the render to the other texture the target is set before the viewport, and its last quad is still
// waiting when it returns.
static char __fastcall panel(void *self, void *edx, void *details, unsigned a, char b, char cc) {
    static const FLOAT one[4] = {1, 1, 1, 1};
    (void)edx;
    seen_ret = __builtin_return_address(0);
    seen_self = self; seen_details = details; seen_a = a; seen_b = b; seen_c = cc;
    if (self == plain) { later(200, 10, 16, 16, 1, 1, 0, 1, over); return 1; }
    ndraws++;
    batch_flush(&pend, NULL);   // its first quad has another state
    ID3D11DeviceContext_OMSetBlendState(c, over, one, 0xFFFFFFFF);
    quad(100, 100, 32, 32, 1, 0, 0, 0.5f, NULL);
    ID3D11DeviceContext_OMSetRenderTargets(c, 1, &side, NULL);   // something of its own rendered elsewhere
    quad(0, 0, W, W, 1, 1, 1, 1, NULL);
    ID3D11DeviceContext_OMSetRenderTargets(c, 1, &bb, NULL);
    quad(116, 116, 32, 32, 0, 1, 0, 0.25f, over);
    quad(96, 122, 40, 12, 0.5f, 0.75f, 1, 1, mul);
    later(108, 108, 16, 16, 0.2f, 0.2f, 0.4f, 1, last_blend == 2 ? dsta : last_blend == 1 ? mul : add);
    return 1;
}

static unsigned char shot[W * W * 4];
static void read_bb(void) {
    D3D11_TEXTURE2D_DESC td;
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11Texture2D *s = NULL;
    ID3D11Texture2D_GetDesc(bbt, &td);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    memset(shot, 0x77, sizeof shot);
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &s))) return;
    ID3D11DeviceContext_CopyResource(c, (ID3D11Resource *)s, (ID3D11Resource *)bbt);
    if (SUCCEEDED(ID3D11DeviceContext_Map(c, (ID3D11Resource *)s, 0, D3D11_MAP_READ, 0, &m))) {
        for (int y = 0; y < W; y++) memcpy(shot + y * W * 4, (unsigned char *)m.pData + y * m.RowPitch, W * 4);
        ID3D11DeviceContext_Unmap(c, (ID3D11Resource *)s, 0);
    }
    ID3D11Texture2D_Release(s);
}

// the frame up to the panel: the scene's colour, the game's states, the whole target as the viewport
static void scene(float r, float g, float b) {
    static const FLOAT one[4] = {1, 1, 1, 1};
    static const D3D11_VIEWPORT full = {0, 0, W, W, 0, 1};
    FLOAT bg[4] = {r, g, b, 1};
    ID3D11DeviceContext_OMSetRenderTargets(c, 1, &bb, NULL);
    game_vp(&full);
    ID3D11DeviceContext_ClearRenderTargetView(c, bb, bg);
    ID3D11DeviceContext_VSSetShader(c, tvs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(c, tps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(c, 0, 1, &cb);
    ID3D11DeviceContext_IASetPrimitiveTopology(c, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_OMSetBlendState(c, over, one, 0xFFFFFFFF);
}

typedef char(__fastcall *DrawFn)(void *self, void *edx, void *details, unsigned a, char b, char c);   // the game's thiscall
// a frame: the scene, the root before the panel (the game's root sets the clip rect to the UI's size), a clipping
// ancestor narrowing the clip rect, the panel (straight, or through the hook), the frame's end
static void frame(float r, float g, float b, void *which, DrawFn draw) {
    scene(r, g, b);
    clip[0] = clip[1] = 0; clip[2] = clip[3] = W;
    draw(plain, NULL, det, 0, 0, 0);
    clip[2] = clip[3] = 100;
    draw(which, NULL, det, 7, 0, 1);
    clip[2] = clip[3] = W;   // popped: the root's rect again
    batch_flush(&pend, NULL);
}

// the largest difference in red, green, blue between the target now and `want`
static int differs(const unsigned char *want) {
    int worst = 0;
    read_bb();
    for (int i = 0; i < W * W * 4; i++)
        if ((i & 3) != 3 && abs(shot[i] - want[i]) > worst) worst = abs(shot[i] - want[i]);
    return worst;
}

static int key_down;   // the test's right mouse button
static SHORT WINAPI test_key(int vk) { return vk == VK_RBUTTON && key_down ? (SHORT)0x8000 : 0; }

int main(int argc, char **argv) {
    if (argc > 1 && strstr(argv[1], ".dll")) return real_dll(argv[1]);
    int addon = argc < 2 || strcmp(argv[1], "plain");   // "plain": without the DLSS add-on's hooks in the vtable
    const char *kind = "hardware";
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, NULL, &c))) {
        kind = "WARP (no hardware device here)";
        if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, NULL, &c))) { printf("FAIL no D3D11 device\n"); return 1; }
    }
    printf("device: %s\n", kind);
    pD3DCompile compile = (pD3DCompile)(void *)GetProcAddress(LoadLibraryA("d3dcompiler_47.dll"), "D3DCompile");
    ID3DBlob *vb = NULL, *pb = NULL;
    CHECK(compile && SUCCEEDED(compile(test_hlsl, strlen(test_hlsl), "t", NULL, NULL, "vs", "vs_5_0", 0, 0, &vb, NULL)) && SUCCEEDED(compile(test_hlsl, strlen(test_hlsl), "t", NULL, NULL, "ps", "ps_5_0", 0, 0, &pb, NULL)));
    if (fails) return 1;
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vb), ID3D10Blob_GetBufferSize(vb), NULL, &tvs);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(pb), ID3D10Blob_GetBufferSize(pb), NULL, &tps);
    D3D11_TEXTURE2D_DESC td = {W, W, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0};
    ID3D11Texture2D *st;
    ID3D11Device_CreateTexture2D(dev, &td, NULL, &bbt);
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)bbt, NULL, &bb);
    ID3D11Device_CreateTexture2D(dev, &td, NULL, &st);
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)st, NULL, &side);
    D3D11_BUFFER_DESC cd = {16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    ID3D11Device_CreateBuffer(dev, &cd, NULL, &cb);
    D3D11_BLEND_DESC bd = {0};   // the alpha factors are the "wrong" ones Attila's game has: they must not matter
    bd.RenderTarget[0] = (D3D11_RENDER_TARGET_BLEND_DESC){TRUE, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD, 15};
    ID3D11Device_CreateBlendState(dev, &bd, &over);
    bd.RenderTarget[0] = (D3D11_RENDER_TARGET_BLEND_DESC){TRUE, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD, 15};
    ID3D11Device_CreateBlendState(dev, &bd, &add);
    bd.RenderTarget[0] = (D3D11_RENDER_TARGET_BLEND_DESC){TRUE, D3D11_BLEND_ZERO, D3D11_BLEND_SRC_COLOR, D3D11_BLEND_OP_ADD, D3D11_BLEND_ZERO, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD, 15};   // Attila's cards' own
    ID3D11Device_CreateBlendState(dev, &bd, &mul);
    bd.RenderTarget[0] = (D3D11_RENDER_TARGET_BLEND_DESC){TRUE, D3D11_BLEND_DEST_ALPHA, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, 15};
    ID3D11Device_CreateBlendState(dev, &bd, &dsta);

    // other mods got to d3d11 first: a hot patch on Draw (a jump in the padding to a detour that counts and goes on past
    // "mov edi, edi") and a 5-byte jump over the start of RSSetViewports
    unsigned char *f = (*(void ***)c)[g_slotof[F_DRAW]], *sm = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE), *mh = sm + 32;
    DWORD prot;
    CHECK(sm && f[0] == 0x8B && f[1] == 0xFF && VirtualProtect(f - 5, 7, PAGE_EXECUTE_READWRITE, &prot));
    if (fails) return 1;
    sm[0] = 0xF0; sm[1] = 0xFF; sm[2] = 0x05; *(volatile LONG **)(sm + 3) = &fw_calls; sm[7] = 0xE9; *(int *)(sm + 8) = (int)(f + 2 - (sm + 12));
    f[-5] = 0xE9; *(int *)(f - 4) = (int)(sm - f); *(unsigned short *)f = 0xF9EB;
    VirtualProtect(f - 5, 7, prot, &prot);
    f = (*(void ***)c)[g_slotof[F_VP]];
    CHECK(!memcmp(f, "\x8B\xFF\x53\x8B\xDC", 5) && VirtualProtect(f, 5, PAGE_EXECUTE_READWRITE, &prot));   // mov edi, edi; push ebx; mov ebx, esp: 5 whole bytes
    if (fails) return 1;
    mh[0] = 0xF0; mh[1] = 0xFF; mh[2] = 0x05; *(volatile LONG **)(mh + 3) = &mh_calls; memcpy(mh + 7, f, 5); mh[12] = 0xE9; *(int *)(mh + 13) = (int)(f + 5 - (mh + 17));
    f[0] = 0xE9; *(int *)(f + 1) = (int)(mh - (f + 5));
    VirtualProtect(f, 5, prot, &prot);

    // and the DLSS add-on's hooks in the vtable, there before the cache patches anything
    void **vt = *(void ***)c;
    static const size_t ad_slot[3] = {offsetof(ID3D11DeviceContextVtbl, OMSetRenderTargets) / sizeof(void *), offsetof(ID3D11DeviceContextVtbl, RSSetViewports) / sizeof(void *),
                                      offsetof(ID3D11DeviceContextVtbl, RSGetViewports) / sizeof(void *)};
    void *const ad_to[3] = {(void *)aw_rt, (void *)aw_vp, (void *)aw_getvp};
    ad_rt = vt[ad_slot[0]]; ad_vp = vt[ad_slot[1]];
    for (int i = 0; i < 3 && addon; i++) {
        CHECK(VirtualProtect(&vt[ad_slot[i]], sizeof(void *), PAGE_READWRITE, &prot));
        vt[ad_slot[i]] = ad_to[i];
        VirtualProtect(&vt[ad_slot[i]], sizeof(void *), prot, &prot);
    }
    if (addon) CHECK(fgb_d3d11_original(ad_slot[0]) == (void *)ad_rt && fgb_d3d11_original(ad_slot[1]) == (void *)ad_vp && !fgb_d3d11_original(g_slotof[F_DRAW]));
    printf("%s\n", addon ? "with the DLSS add-on's hooks in the context's vtable" : "without the add-on's hooks");

    // the game's function: its first instruction as in the game (mov eax, [abs]), then the draw above
    unsigned char *code = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    code[0] = 0xA1; *(DWORD **)(code + 1) = &first_global;
    code[5] = 0xE9; *(int *)(code + 6) = (int)((unsigned char *)(void *)panel - (code + 10));
    DrawFn game = (DrawFn)(void *)code;
    CHECK(install_uicache(NULL, "card_panel_docker\nother_panel", code, tlog) == 1 && g_nt == 2 && code[0] == 0xE9);
    g_ctx = c; g_flush = batch_flush;
    g_clip = clip; g_root_wh = g_ui_wh;   // as in game: the UI's size from the clip rect at the root's Draw
    static unsigned char ref1[W * W * 4], ref2[W * W * 4];
    state72[0x28 / 4] = state72[0x2C / 4] = 72;
    *(const char **)(comp + C_NAME) = "card_panel_docker";
    *(const char **)(comp2 + C_NAME) = "other_panel";
    *(const char **)(plain + C_NAME) = "root";
    *(const char **)(hud + C_NAME) = "hud";
    *(void **)(comp + C_PARENT) = *(void **)(comp2 + C_PARENT) = hud;   // the panels are not roots; plain is
    *(float **)(comp + C_STATE) = *(float **)(comp2 + C_STATE) = state72;
    comp[C_VISIBLE] = comp2[C_VISIBLE] = plain[C_VISIBLE] = 1;
    ((float *)(comp + C_POS))[0] = ((float *)(comp + C_POS))[1] = 96;     // 96..168: the quads are inside, up to 12 px out
    ((float *)(comp2 + C_POS))[0] = ((float *)(comp2 + C_POS))[1] = 96;

    // 1. the panel drawn straight onto two backgrounds: what it has to look like
    frame(0.2f, 0.4f, 0.6f, comp, panel); read_bb(); memcpy(ref1, shot, sizeof shot);
    frame(0.5f, 0.1f, 0.3f, comp, panel); read_bb(); memcpy(ref2, shot, sizeof shot);
    CHECK(memcmp(ref1, ref2, sizeof ref1) && ref1[(112 * W + 112) * 4] != ref1[(2 * W + 2) * 4] && ref1[(18 * W + 208) * 4] == 255);   // the quads are there, the one before the panel too

    // 2. a component that is not cached: the game's function gets the caller's own return address and arguments, its
    // first instruction ran, and nothing of d3d11 has been touched yet
    int n0 = ndraws, worst;
    scene(0.2f, 0.4f, 0.6f);
    first_global = 0x1234;
    CHECK(game(plain, NULL, det, 0x12345678, 1, 0) == 1 && ndraws == n0 && pend.on);
    void *here = seen_ret;
    CHECK(seen_self == plain && seen_details == det && seen_a == 0x12345678 && seen_b == 1 && seen_c == 0);
    HMODULE owner = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, here, &owner);
    CHECK(owner == GetModuleHandleW(NULL) && g_nfn == 0);   // it returns into this program, not into a stub
    CHECK(g_ui_wh[0] == W && g_ui_wh[1] == W);   // the root's clip rect
    batch_flush(&pend, NULL);

    // 3. through the cache: drawn into the textures once, the same picture; the textures are the panel's size (in the
    // root's units, not the narrowed clip rect's); the other mods' hooks saw the panel's draws (each twice: black and
    // white) and viewports; what the game finds bound
    n0 = ndraws;
    LONG fw0 = fw_calls, mh0 = mh_calls;
    g_frame_no++;
    frame(0.2f, 0.4f, 0.6f, comp, game);
    CHECK(ndraws - n0 == 1 && seen_self == comp && seen_details == det && seen_a == 7 && seen_c == 1);
    worst = differs(ref1);
    printf("drawn into the cache and shown: largest difference %d of 255; textures %ux%u at %d,%d; %d d3d11 functions patched\n", worst, g_t[0].tw, g_t[0].th, g_t[0].x,
           g_t[0].y, g_nfn);
    CHECK(worst <= 2 && g_t[0].valid && !g_t[0].off && g_t[0].why[1] == 1);
    CHECK(g_t[0].tw == 192 && g_t[0].th == 192 && g_t[0].x == 48 && g_t[0].y == 48);   // 96 - MARGIN .. 168 + MARGIN, in steps of 64
    CHECK(fw_calls - fw0 == 1 + 5 + 4 + 1 && mh_calls - mh0 >= 7);   // Draw: the one before the panel, its 5 quads, 4 of them again, ours
    for (int i = 0; i < g_nfn; i++) CHECK(g_uic_go[i] == g_next[i]);   // the patches lead straight on again
    ID3D11BlendState *bs = NULL; ID3D11RenderTargetView *rt = NULL; ID3D11VertexShader *v = NULL; ID3D11PixelShader *p = NULL;
    D3D11_VIEWPORT vp; UINT nvp = 1, mask; FLOAT bf[4];
    ID3D11DeviceContext_OMGetBlendState(c, &bs, bf, &mask);
    ID3D11DeviceContext_OMGetRenderTargets(c, 1, &rt, NULL);
    ID3D11DeviceContext_VSGetShader(c, &v, NULL, NULL);
    ID3D11DeviceContext_PSGetShader(c, &p, NULL, NULL);
    ID3D11DeviceContext_RSGetViewports(c, &nvp, &vp);
    CHECK(bs == add && rt == bb && v == tvs && p == tps && vp.TopLeftX == 108 && vp.Width == 16);   // as the panel's own draws left it
    REL(bs); REL(rt); REL(v); REL(p);

    // 4. the next frame over another background: not drawn again, still the right picture
    g_frame_no++;
    frame(0.5f, 0.1f, 0.3f, comp, game);
    CHECK(ndraws - n0 == 1 && g_t[0].reuse == 1);
    worst = differs(ref2);
    printf("shown from the cache over another background: largest difference %d of 255\n", worst);
    CHECK(worst <= 2);

    // 5. it holds while d3d11 swaps its functions: 300 frames, a Flush in each, drawn again whenever the details change
    int bad = 0;
    for (int i = 0; i < 300; i++) {
        g_frame_no++;
        ID3D11DeviceContext_Flush(c);
        det[0x14] = (unsigned char)(i / 3);
        frame(i & 1 ? 0.5f : 0.2f, i & 1 ? 0.1f : 0.4f, i & 1 ? 0.3f : 0.6f, comp, game);
        bad += differs(i & 1 ? ref2 : ref1) > 2;
    }
    CHECK(bad == 0 && ndraws - n0 >= 1 + 99 && !g_t[0].off);   // 99 changes of the details; more if the machine was slow (the interval)
    printf("300 frames with a Flush each: %d wrong, drawn again %d times, %d d3d11 functions patched\n", bad, ndraws - n0 - 1, g_nfn);

    // 6. the redraw rules: after INTERVAL ms; when it has moved; with the cache off it is drawn the game's way every time
    g_frame_no++;
    frame(0.2f, 0.4f, 0.6f, comp, game);
    n0 = ndraws;
    g_frame_no++;
    frame(0.2f, 0.4f, 0.6f, comp, game);   // right after: from the textures
    CHECK(ndraws == n0);
    Sleep(INTERVAL + 30);
    g_frame_no++;
    frame(0.2f, 0.4f, 0.6f, comp, game);
    CHECK(ndraws - n0 == 1 && differs(ref1) <= 2);
    ((float *)(comp + C_POS))[0] = 90;   // moved: drawn again, the textures' corner with it
    g_frame_no++;
    frame(0.2f, 0.4f, 0.6f, comp, game);
    CHECK(ndraws - n0 == 2 && g_t[0].x == 42 && differs(ref1) <= 2);
    state72[0x28 / 4] = 70;              // another size: drawn again
    g_frame_no++;
    frame(0.2f, 0.4f, 0.6f, comp, game);
    CHECK(ndraws - n0 == 3 && g_t[0].rect[2] == 70 && differs(ref1) <= 2);
    state72[0x28 / 4] = 72;
    g_on = 0;
    g_frame_no++;
    frame(0.2f, 0.4f, 0.6f, comp, game);
    CHECK(ndraws - n0 == 4 && differs(ref1) == 0);
    g_on = 1;
    comp[C_VISIBLE] = 0;   // hidden: left to the game
    g_frame_no++;
    frame(0.2f, 0.4f, 0.6f, comp, game);
    CHECK(ndraws - n0 == 5);
    comp[C_VISIBLE] = 1;

    // 7. a panel with a blend that reads the target's alpha: found out at its first draw, the game's way from then on
    last_blend = 2;
    frame(0.2f, 0.4f, 0.6f, comp2, panel); read_bb(); memcpy(ref1, shot, sizeof shot);
    n0 = ndraws;
    g_frame_no++;
    frame(0.2f, 0.4f, 0.6f, comp2, game);
    CHECK(g_t[1].off == 1 && ndraws - n0 == 1);
    for (int i = 0; i < 3; i++) {
        g_frame_no++;
        frame(0.2f, 0.4f, 0.6f, comp2, game);
        CHECK(differs(ref1) == 0);
    }
    CHECK(ndraws - n0 == 4);
    last_blend = 0;

    // 7b. an ancestor asked for is looked for up the parents
    static unsigned char orders[0x200], card[0x200], lone[0x200];
    *(const char **)(orders + C_NAME) = "battle_orders";
    *(const char **)(card + C_NAME) = *(const char **)(lone + C_NAME) = "card_panel_docker";
    *(void **)(card + C_PARENT) = orders;
    CHECK(is_under(card, "battle_orders") && !is_under(lone, "battle_orders") && !is_under(orders, "battle_orders"));

    // 7f. a held button is input when it goes down and when it comes up, not in between: the frame right after an
    // interval's redraw comes from the textures although the button is still down
    g_key = test_key; g_test_keys = 1;
    frame(0.2f, 0.4f, 0.6f, comp, panel); read_bb(); memcpy(ref1, shot, sizeof shot);   // what it has to look like
    g_frame_no++;
    frame(0.2f, 0.4f, 0.6f, comp, game);
    Sleep(INTERVAL + 30);
    g_frame_no++;
    frame(0.2f, 0.4f, 0.6f, comp, game);
    n0 = ndraws;
    g_frame_no++;
    frame(0.2f, 0.4f, 0.6f, comp, game);
    CHECK(ndraws == n0);                       // nothing down: from the textures
    key_down = 1;
    g_frame_no++;
    frame(0.2f, 0.4f, 0.6f, comp, game);
    CHECK(ndraws - n0 == 1);                   // it went down: drawn again
    Sleep(300);                                // held, past the 250 ms
    g_frame_no++;
    frame(0.2f, 0.4f, 0.6f, comp, game);       // the interval's redraw
    g_frame_no++;
    frame(0.2f, 0.4f, 0.6f, comp, game);
    CHECK(ndraws - n0 == 2);                   // still down, right after: from the textures
    key_down = 0;
    g_frame_no++;
    frame(0.2f, 0.4f, 0.6f, comp, game);
    CHECK(ndraws - n0 == 3 && differs(ref1) <= 2);   // it came up: drawn again
    g_key = GetAsyncKeyState; g_test_keys = 0;

    // 8. what the flush is for: without it the two quads that are still waiting go wrong, in a redrawn frame (the one
    // before the panel lands in the textures, outside them) or in the next (the panel's last is not in the textures)
    frame(0.2f, 0.4f, 0.6f, comp, panel); read_bb(); memcpy(ref1, shot, sizeof shot);
    g_flush = NULL;
    g_t[0].valid = 0;
    g_frame_no++;
    frame(0.2f, 0.4f, 0.6f, comp, game);
    int wrong1 = differs(ref1);
    g_frame_no++;
    frame(0.2f, 0.4f, 0.6f, comp, game);
    int wrong2 = differs(ref1);
    printf("without the flush: %d and %d of 255 off in two frames\n", wrong1, wrong2);
    CHECK(wrong1 > 50 && wrong2 > 50);
    g_flush = batch_flush;

    ID3D11DeviceContext_Flush(c);
    // the add-on's hooks saw the game's calls, and nothing of the cache's own
    printf("the add-on's hooks in the vtable: %d target and %d viewport calls seen, %d and %d of them not the game's\n", ad_rt_calls, ad_vp_calls, ad_foreign_rt, ad_foreign_vp);
    if (addon) CHECK(ad_rt_calls > 300 && ad_vp_calls > 300 && !ad_foreign_rt && !ad_foreign_vp && vt[ad_slot[0]] == (void *)aw_rt && vt[ad_slot[1]] == (void *)aw_vp);
    else CHECK(!ad_rt_calls && !ad_vp_calls);
    CHECK(ID3D11Device_GetDeviceRemovedReason(dev) == S_OK);
    if (!fails) printf("uicache test passed\n");
    return fails ? 1 : 0;
}
