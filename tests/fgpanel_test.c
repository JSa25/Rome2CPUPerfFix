// Test for fgpanel.c on a WARP device (no game, no helper):
// - after a resolution change on the same device the panel takes the back buffer's size from the frame (Attila audit
//   2026-11-20: the mouse was scaled by the old size, clicks landed beside the cursor);
// - a pop-up goes after its time while the game window is not in front (Attila AGENT_LOG 227: counting only in front
//   kept it up for good);
// - the pop-ups come one at a time, a newer version first, then What's new, then a note; opening the panel drops them;
// - the What's new page and its Back button.
// usage: fgpanel_test.exe
#include "../fgpanel.c"

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

// fgbridge.c's side, as with no helper running
UINT fgb_render_w(void) { return 0; }
UINT fgb_render_h(void) { return 0; }
unsigned fgb_fg_max(void) { return 2; }
unsigned fgb_sr_backend(void) { return 1; }
unsigned fgb_fg_backend(void) { return 1; }
void fgb_gpu_caps(int *dlss_sr, int *dlss_fg) { *dlss_sr = *dlss_fg = 1; }
const wchar_t *fgb_gpu_name(void) { return L"NVIDIA GeForce RTX 4070"; }

// the culling's counters and the pick, as proxy.c hands them over
static unsigned g_parts, g_skipped, g_forced;
static void cull_counts(unsigned *p, unsigned *s, unsigned *f) { *p = g_parts; *s = g_skipped; *f = g_forced; }
static void cull_apply(int c) { (void)c; }
static int g_menus;
static int menus_up(void) { return g_menus; }
static int find_btn(int row, int value) {
    for (int i = 0; i < p_nbtn; i++) if (p_btn[i].row == row && p_btn[i].value == value) return i;
    return -1;
}

static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static ID3D11Texture2D *backbuffer(UINT w, UINT h) {
    D3D11_TEXTURE2D_DESC td = {w, h, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                               D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
    ID3D11Texture2D *t = NULL;
    CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &t)));
    return t;
}
// one pop-up shown and timed out on a window that is never in front: its kind
static int next_popup(ID3D11Resource *bb, HWND hwnd) {
    fgp_frame(bb, hwnd);
    if (t_show != 2) return 0;
    int kind = t_kind;
    t_end = GetTickCount64() + 50;   // its 20 s, cut short
    for (int i = 0; i < 40 && t_show; i++) { Sleep(10); fgp_frame(bb, hwnd); }
    CHECK(!t_show);   // gone with the window not in front
    return kind;
}

int main(void) {
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    CHECK(SUCCEEDED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, &fl, 1, D3D11_SDK_VERSION, &dev, NULL, &ctx)));
    // a hidden window stands in for the game's: never the foreground window
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"fgpanel_test", WS_POPUP, 0, 0, 640, 360, NULL, NULL, NULL, NULL);
    CHECK(hwnd && GetForegroundWindow() != hwnd);
    FgpApply apply = {0};
    CHECK(fgp_init(dev, ctx, 1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM, 2, 0, 1, apply));
    ID3D11Texture2D *bb1 = backbuffer(1920, 1080), *bb2 = backbuffer(2560, 1440);

    fgp_frame((ID3D11Resource *)bb1, hwnd);
    CHECK(p_bw == 1920 && p_bh == 1080);
    fgp_frame((ID3D11Resource *)bb2, hwnd);   // the game switched to 1440p on the same device
    CHECK(p_bw == 2560 && p_bh == 1440);
    printf("back buffer size followed: 1920x1080 -> %ux%u\n", p_bw, p_bh);

    // all three waiting at once: one at a time, in this order, each gone after its time off-focus
    fgp_set_note(L"The DLSS add-on did not start. Choices made here are saved.");
    fgp_show_whatsnew();
    fgp_set_update(1);
    int k1 = next_popup((ID3D11Resource *)bb2, hwnd), k2 = next_popup((ID3D11Resource *)bb2, hwnd), k3 = next_popup((ID3D11Resource *)bb2, hwnd);
    printf("pop-ups in order: %d %d %d\n", k1, k2, k3);
    CHECK(k1 == 2 && k2 == 4 && k3 == 1);
    fgp_frame((ID3D11Resource *)bb2, hwnd);
    CHECK(!t_show && !t_queue);

    // opening the panel drops what still waits; the What's new page and back
    fgp_show_whatsnew();
    fgp_set_update(2);
    fgp_test_open(1);
    fgp_frame((ID3D11Resource *)bb2, hwnd);
    CHECK(!t_show && !t_queue);
    int wn = -1, back = -1, defaults = -1;
    for (int i = 0; i < p_nbtn; i++) { if (p_btn[i].row == 24) wn = i; if (p_btn[i].row == 20) defaults = i; }
    CHECK(wn >= 0 && defaults >= 0);
    p_page = 2; p_dirty = 1;   // what a click on What's new does (fgp_frame, row 24)
    fgp_frame((ID3D11Resource *)bb2, hwnd);
    for (int i = 0; i < p_nbtn; i++) { if (p_btn[i].row == 23) back = i; if (p_btn[i].row == 20 || p_btn[i].row == 24) defaults = -2; }
    CHECK(back >= 0 && defaults != -2);   // What's new: Back, no Defaults and no What's new button
    CHECK(p_cy1 < p_cy1_full);           // the card shrinks to the page
    p_page = 0; p_dirty = 1;
    fgp_frame((ID3D11Resource *)bb2, hwnd);
    CHECK(p_cy1 == p_cy1_full);
    printf("What's new page: Back button, card %s\n", "shrunk and restored");

    // Culling: the header button on the upscaling page, its page with the four choices and Back, the live share
    fgp_set_cull_source(cull_counts, cull_apply);
    fgp_set_cull_available(1);
    fgp_set_cull(1);
    p_dirty = 1;
    fgp_frame((ID3D11Resource *)bb2, hwnd);
    CHECK(find_btn(22, 0) >= 0);
    p_page = 1; p_dirty = 1;   // what a click on Culling does (row 22)
    fgp_frame((ID3D11Resource *)bb2, hwnd);
    CHECK(find_btn(21, 3) >= 0 && p_btn[find_btn(21, 3)].enabled && find_btn(23, 0) >= 0 && find_btn(22, 0) < 0);
    CHECK(p_cy1 < p_cy1_full);
    float share, forced;
    g_parts = 1000; g_skipped = 0;
    Sleep(600);
    cull_share(&share, &forced);   // the baseline (the page's redraw above took the first reading)
    Sleep(600);
    g_parts = 2000; g_skipped = 400; g_forced = 100;
    CHECK(cull_share(&share, &forced) && share > 39.9f && share < 40.1f && forced > 9.9f && forced < 10.1f);
    fgp_set_cull_available(0);   // not on this game version: the choices are off
    p_dirty = 1;
    fgp_frame((ID3D11Resource *)bb2, hwnd);
    CHECK(find_btn(21, 3) >= 0 && !p_btn[find_btn(21, 3)].enabled);
    fgp_set_cull_only();   // no add-on: no Back to an upscaling page
    p_dirty = 1;
    fgp_frame((ID3D11Resource *)bb2, hwnd);
    CHECK(p_page == 1 && find_btn(23, 0) < 0);
    printf("Culling page: choices, Back, live share %.0f %% (%.0f %% placed by the engine)\n", share, forced);

    // the main menu label: drawn only while the menus are up
    g_menus = 0;
    g_fgp_menu_label = menus_up;
    fgp_frame((ID3D11Resource *)bb2, hwnd);
    CHECK(!l_srv);
    g_menus = 1;
    fgp_frame((ID3D11Resource *)bb2, hwnd);
    CHECK(l_srv && !l_bad);
    printf("main menu label: drawn while the menus are up only\n");
    printf("all fgpanel tests passed\n");
    return 0;
}
