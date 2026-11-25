// Test for overlay.c on a WARP device (no game, no add-on): the test exe stands in for empire.retail.dll (its own
// GetProcAddress import is hooked), and goes the game's way: GetProcAddress("CreateDXGIFactory1") -> factory ->
// CreateSwapChain -> Present. Checks: the panel is set up once (fgp_init on the first frame, F8 = Culling) and drawn over
// the frame; another mod hooking Present on top of ours (like the Steam overlay) and a second swapchain do not get
// Present hooked twice (the two would call each other forever); a new device moves the panel instead of a second
// fgp_init; the game's DirectInput goes to the panel. A broken copy (slots patched again) must fail it.
// usage: overlay_test.exe
#include "../fgpanel.c"
#define FGPANEL_IN_TU
#include "../overlay.c"
#include <stdarg.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

void (*g_dev_di8_created)(void *di);   // proxy.c's
// fgbridge.c's side, as with no helper running
UINT fgb_render_w(void) { return 0; }
UINT fgb_render_h(void) { return 0; }
unsigned fgb_fg_max(void) { return 2; }
unsigned fgb_sr_backend(void) { return 0; }
unsigned fgb_fg_backend(void) { return 0; }
void fgb_gpu_caps(int *dlss_sr, int *dlss_fg) { *dlss_sr = *dlss_fg = 0; }
const wchar_t *fgb_gpu_name(void) { return L"Microsoft Basic Render Driver"; }

static int n_ready, n_logs;
static void tlog(const char *fmt, ...) {
    char s[512];
    va_list a;
    va_start(a, fmt);
    vsnprintf(s, sizeof s, fmt, a);
    va_end(a);
    printf("  log: %s\n", s);
    n_logs++;
    n_ready += !strncmp(s, "F8 panel ready", 14);
}
static const GUID iid_factory1 = {0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};
typedef HRESULT(WINAPI *factory_fn)(REFIID, void **);
// another mod's Present hook put on top of ours: calls what the slot held as its original
static HRESULT(STDMETHODCALLTYPE *f_orig)(IDXGISwapChain *, UINT, UINT);
static unsigned f_calls;
static HRESULT STDMETHODCALLTYPE f_present(IDXGISwapChain *sc, UINT s, UINT fl) { f_calls++; return f_orig(sc, s, fl); }

static IDXGISwapChain *swapchain(factory_fn fac_fn, ID3D11Device *dev, HWND hwnd) {
    IDXGIFactory *fac = NULL;
    CHECK(SUCCEEDED(fac_fn(&iid_factory1, (void **)&fac)));
    DXGI_SWAP_CHAIN_DESC scd = {{1280, 720, {60, 1}, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 0}, {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT,
                                1, hwnd, TRUE, DXGI_SWAP_EFFECT_SEQUENTIAL, 0};
    IDXGISwapChain *sc = NULL;
    CHECK(SUCCEEDED(IDXGIFactory_CreateSwapChain(fac, (IUnknown *)dev, &scd, &sc)));   // through the factory's table
    IDXGIFactory_Release(fac);
    return sc;
}
static ID3D11Device *warp(ID3D11DeviceContext **ctx) {
    ID3D11Device *dev = NULL;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    CHECK(SUCCEEDED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, &fl, 1, D3D11_SDK_VERSION, &dev, NULL, ctx)));
    return dev;
}
// one pixel of the swapchain's back buffer (RGBA)
static unsigned pixel(IDXGISwapChain *sc, ID3D11Device *dev, ID3D11DeviceContext *ctx, UINT x, UINT y) {
    ID3D11Texture2D *bb = NULL, *st = NULL;
    CHECK(SUCCEEDED(IDXGISwapChain_GetBuffer(sc, 0, &iid_tex2d, (void **)&bb)));
    D3D11_TEXTURE2D_DESC td = {1, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
    CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &st)));
    D3D11_BOX box = {x, y, 0, x + 1, y + 1, 1};
    ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)st, 0, 0, 0, 0, (ID3D11Resource *)bb, 0, &box);
    D3D11_MAPPED_SUBRESOURCE ms;
    CHECK(SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &ms)));
    unsigned v = *(unsigned *)ms.pData;
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
    ID3D11Texture2D_Release(st);
    ID3D11Texture2D_Release(bb);
    return v;
}
static void clear_green(IDXGISwapChain *sc, ID3D11Device *dev, ID3D11DeviceContext *ctx) {
    ID3D11Texture2D *bb = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    CHECK(SUCCEEDED(IDXGISwapChain_GetBuffer(sc, 0, &iid_tex2d, (void **)&bb)));
    CHECK(SUCCEEDED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)bb, NULL, &rtv)));
    const float green[4] = {0, 1, 0, 1};
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, green);
    ID3D11RenderTargetView_Release(rtv);
    ID3D11Texture2D_Release(bb);
}

int main(void) {
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"overlay_test", WS_POPUP, 0, 0, 1280, 720, NULL, NULL, NULL, NULL);
    CHECK(hwnd);
    CHECK(install_overlay(GetModuleHandleW(NULL), tlog) == 1);
    CHECK(g_dev_di8_created == fgp_di8_created);
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    factory_fn fac_fn = (factory_fn)(void *)GetProcAddress(dxgi, "CreateDXGIFactory1");   // the game's look-up
    CHECK(fac_fn == w_factory);
    CHECK((factory_fn)(void *)GetProcAddress(dxgi, "CreateDXGIFactory") != w_factory);   // others untouched

    ID3D11DeviceContext *ctx = NULL;
    ID3D11Device *dev = warp(&ctx);
    IDXGISwapChain *sc = swapchain(fac_fn, dev, hwnd);
    void **vt = *(void ***)sc, **slot = &vt[offsetof(IDXGISwapChainVtbl, Present) / sizeof(void *)];
    CHECK(*slot == (void *)w_present && r_present && (void *)r_present != (void *)w_present);
    void *real = (void *)r_present;
    CHECK(n_ready == 0 && !p_ok);   // nothing before the first frame

    // first frame: the panel set up (Culling page, F8 only), and drawn over the game's frame while open
    clear_green(sc, dev, ctx);
    fgp_test_open(1);
    CHECK(SUCCEEDED(IDXGISwapChain_Present(sc, 0, 0)));
    CHECK(n_ready == 1 && p_ok && p_dev == dev && p_cullonly && p_page == 1);
    unsigned in = pixel(sc, dev, ctx, 1280 * 3 / 100 + 40, 720 * 8 / 100 + 40), out = pixel(sc, dev, ctx, 1200, 680);
    printf("panel over the frame: 0x%08X inside, 0x%08X outside (green 0xFF00FF00)\n", in, out);
    CHECK(out == 0xFF00FF00u && in != 0xFF00FF00u);
    for (int i = 0; i < 3; i++) CHECK(SUCCEEDED(IDXGISwapChain_Present(sc, 0, 0)));
    CHECK(n_ready == 1);

    // another mod hooks Present on top of ours; the game makes a second swapchain (a new factory too): ours stays below
    f_orig = (HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *, UINT, UINT))*slot;
    DWORD prot;
    CHECK(VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &prot));
    *slot = (void *)f_present;
    VirtualProtect(slot, sizeof *slot, prot, &prot);
    factory_fn again = (factory_fn)(void *)GetProcAddress(dxgi, "CreateDXGIFactory1");
    IDXGISwapChain *sc2 = swapchain(again, dev, hwnd);
    CHECK(*(void ***)sc2 == vt);   // one table for every swapchain
    CHECK(*slot == (void *)f_present && (void *)r_present == real);   // not hooked twice
    for (int i = 0; i < 3; i++) CHECK(SUCCEEDED(IDXGISwapChain_Present(sc2, 0, 0)));
    CHECK(f_calls == 3 && n_ready == 1);

    // a new device (a settings change): the panel moves to it, no second set-up
    ID3D11DeviceContext *ctx3 = NULL;
    ID3D11Device *dev3 = warp(&ctx3);
    IDXGISwapChain *sc3 = swapchain(again, dev3, hwnd);
    CHECK(SUCCEEDED(IDXGISwapChain_Present(sc3, 0, 0)));
    CHECK(n_ready == 1 && p_ok && p_dev == dev3);
    printf("overlay: OK (fgp_init once on the first frame, Culling page, Present hooked once under another mod's hook, "
           "panel moved to a new device, %d log lines)\n", n_logs);
    return 0;
}
