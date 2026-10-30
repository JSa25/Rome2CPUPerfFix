// Test for the live frame bridge glue in bridgeprobe.c: the test exe stands in for empire.retail.dll and renders frames the
// way the captures show Rome 2 does (dlss/DLSS_LOG.md 4/5): 4 draws into the back buffer before the scene, a shadow pass
// (own depth, its camera written), the scene (swapchain-sized R24G8 depth, the main camera written), a reflection camera
// written with no depth bound, the scene's copy into the back buffer (blue), the UI on top (red), Present.
// Default build: fgbridge.c is replaced by the stubs below, which check what the glue hands over: the HUD-less copy is
// taken after the blue copy and before the red UI, with this frame's main camera; the game's swapchain goes to a hidden
// window, windowed, its fullscreen requests answered; MSAA depth frames give no HUD-less copy; a helper that dies ends
// the bridge (fgb_stop) and the game's frames go to the fallback.
// -DREAL_BRIDGE (with fgbridge.c linked): the real helper; usage: bridgelive_test.exe <helper64.exe> <streamline dir>
//   checks that the helper shows 2 frames per Present with frame generation 2x and that every frame had a HUD-less copy.
#include "../bridgeprobe.c"
#include <stdarg.h>
#include <math.h>
#ifdef PROFILING
#define DEV_VIEW 1   // dev builds: the debug view (F11 panel)
#else
#define DEV_VIEW 0   // the release form: none
#endif

void (*g_dev_di8_created)(void *di);   // proxy.c's, in the DLL
static int saw_upscale __attribute__((unused));   // fgbridge.c logged its depth pass (below native size; real helper only)
static void tlog(const char *fmt, ...) {
    if (strstr(fmt, "depth buffer back to full size")) saw_upscale = 1;
    va_list a;
    va_start(a, fmt);
    vprintf(fmt, a);
    va_end(a);
    putchar('\n');
}
static const GUID iid_factory1 = {0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};
static const GUID iid_tex2d = {0x6f15aaf2, 0xd208, 0x4e89, {0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c}};
#define CHECK(c, ...) do { if (!(c)) { printf("FAILED: " __VA_ARGS__); putchar('\n'); return 1; } } while (0)
#ifdef REAL_BRIDGE   // like bridgetest: frame generation shows in a normal-sized foreground window (perf 4k: the screen's size)
static unsigned W = 1280, H = 720;
#else
#define W 320
#define H 180
#endif
static int g_fr;   // the test's frame number
// the main camera: a perspective view panning along x, row-major, clip = M x [x y z 1] like Rome 2's (an invertible matrix:
// fgbridge.c skips a frame's motion vectors, and the helper its generated frames, when it cannot invert the camera)
static float main_vp(int fr, int i) {
    const float f = 1.7f, a = (float)W / H, n = 1, fa = 1000, tx = -0.01f * fr;
    const float m[16] = {f / a, 0, 0, f / a * tx, 0, f, 0, 0, 0, 0, fa / (fa - n), -n * fa / (fa - n), 0, 0, 1, 0};
    return m[i];
}

#ifndef REAL_BRIDGE
#include "../r2motionvs.h"
static int s_started, s_active = 1, s_stopped, s_hud_bad, s_present_n, s_fallback_n;
static UINT s_rw = W, s_rh = H;   // the render size the glue asked for (fgb_set_sr)
static unsigned s_hud_n;
static HWND s_hwnd;
static ID3D11Texture2D *s_stage;
static void stub_motion(ID3D11Resource *depth);
int fgb_start(ID3D11Device *dev, ID3D11DeviceContext *ctx, HWND hwnd, UINT w, UINT h, DXGI_FORMAT fmt, const wchar_t *helper,
              const wchar_t *sl, const wchar_t *logdir, unsigned fg, void (*log)(const char *, ...)) {
    (void)dev; (void)ctx; (void)fmt; (void)helper; (void)sl; (void)logdir; (void)fg; (void)log;
    s_started++;
    s_hwnd = hwnd;
    return w == W && h == H;
}
// the HUD-less copy: the back buffer's centre must be the scene's blue copy (not the red UI, not last frame's picture)
// and vp this frame's main camera (not the shadow or reflection camera)
void fgb_hudless(ID3D11Resource *bb, ID3D11Resource *depth, const float vp[16]) {
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)depth, &d);
    if (!s_stage) {
        D3D11_TEXTURE2D_DESC s = {W, H, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
        ID3D11Device_CreateTexture2D(g_dev, &s, NULL, &s_stage);
    }
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)s_stage, bb);
    D3D11_MAPPED_SUBRESOURCE m;
    unsigned px = 0;
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)s_stage, 0, D3D11_MAP_READ, 0, &m))) {
        px = *(const unsigned *)((const char *)m.pData + (s_rh / 2) * m.RowPitch + (s_rw / 2) * 4);   // the scene: top-left render size
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)s_stage, 0);
    }
    int ok = px == 0xFFFF0000u && d.Width == W && d.Height == H && d.SampleDesc.Count == 1;
    if (s_rw != W) {   // below native size fgbridge.c restores the depth through the bound view: it must be writable, of the scene depth
        ID3D11DepthStencilView *bv = NULL;
        ID3D11DeviceContext_OMGetRenderTargets(g_ctx, 0, NULL, &bv);
        D3D11_DEPTH_STENCIL_VIEW_DESC vd = {0};
        ID3D11Resource *vr = NULL;
        if (bv) { ID3D11DepthStencilView_GetDesc(bv, &vd); ID3D11DepthStencilView_GetResource(bv, &vr); }
        if (!bv || vd.Flags || vr != depth) {
            ok = 0;
            printf("  depth view bound at frame %d: %p flags 0x%x (read-only views drop the restore's writes)\n", g_fr, (void *)bv, vd.Flags);
        }
        if (vr) ID3D11Resource_Release(vr);
        if (bv) ID3D11DepthStencilView_Release(bv);
    }
    for (int i = 0; i < 16; i++) ok &= vp[i] == main_vp(g_fr, i);
    if (!ok && s_hud_bad++ < 3) printf("  bad HUD-less copy at frame %d: centre %08x, depth %ux%u x%u, vp[0] %g (main %g)\n", g_fr, px, d.Width,
                                       d.Height, d.SampleDesc.Count, vp[0], main_vp(g_fr, 0));
    s_hud_n++;
    stub_motion(depth);
}
// the motion pass (r2motion.c, linked): fgbridge.c runs it after its camera motion; here into a cleared texture, read
// back at the Rome 2-like model (r2_check below)
static void (*s_motion)(ID3D11Resource *depth, float jx, float jy);
static ID3D11Texture2D *s_mv, *s_mv_st;
static ID3D11RenderTargetView *s_mv_rtv;
static int s_mv_checks, s_mv_bad;
static int r2_check(void);
void fgb_set_motion(void (*pass)(ID3D11Resource *depth, float jx, float jy)) { s_motion = pass; }
ID3D11Texture2D *fgb_mv_texture(void) {
    if (!s_mv) {
        D3D11_TEXTURE2D_DESC d = {W, H, 1, 1, DXGI_FORMAT_R16G16_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};   // as fgbridge.c's
        ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &s_mv);
        ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)s_mv, NULL, &s_mv_rtv);
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &s_mv_st);
    }
    return s_mv;
}
static void stub_motion(ID3D11Resource *depth) {
    if (!s_motion) return;
    float zero[4] = {g_fr == 80 ? 50.0f : 0, 0, 0, 0}, jx, jy;   // frame 80: a whole-screen jump for the flash probe
    ID3D11DeviceContext_ClearRenderTargetView(g_ctx, s_mv_rtv, zero);
    fgb_jitter(&jx, &jy);
    s_motion(depth, jx, jy);
    if ((g_fr == 60 || g_fr == 180) && !r2_check() && s_mv_bad++ < 3) printf("  wrong object motion at frame %d\n", g_fr);
    s_mv_checks += g_fr == 60 || g_fr == 180;
}
static int s_view_ok = -1;
static int view_check(ID3D11Resource *bb);
void fgb_present(ID3D11Resource *bb) {   // 80: the fake whole-screen jump must not tint the view (the models' own motion only)
    s_present_n++;
    if (g_fr == 60 || g_fr == 80) s_view_ok = (g_fr == 60 || s_view_ok == 1) && view_check(bb);
}
int fgb_active(void) { return s_active; }
unsigned fgb_frames(void) { return (unsigned)s_present_n; }
unsigned fgb_pace_timeouts(void) { return 0; }
static unsigned s_fg_max = 1;   // "nofg": a GPU without frame generation (RTX 20 / 30)
unsigned fgb_fg_max(void) { return s_fg_max; }
void fgb_stop(void) { s_stopped++; }
IDXGISwapChain *fgb_fallback_frame(ID3D11Resource *src, HRESULT(STDMETHODCALLTYPE *create)(IDXGIFactory *, IUnknown *, DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **)) {
    (void)src; (void)create;
    s_fallback_n++;
    return NULL;   // not this frame: the glue returns S_OK
}
void fgb_fallback_close(void) {}
void *fgb_vt_original(void **vt, size_t slot) { (void)vt; (void)slot; return NULL; }
void *fgb_unhooked(void *fn) { return fn; }
void fgb_set_test_not_nvidia(int on) { (void)on; }
void fgb_cpu_stats(double *hud_ms, double *present_ms, double *pace_ms, unsigned *frames) { *hud_ms = *present_ms = *pace_ms = 0; *frames = 0; }
static int s_sr, s_panel_init, s_panel_frames;
void fgb_set_sr(int on, UINT w, UINT h) { s_sr = on; s_rw = on ? w : W; s_rh = on ? h : H; }
unsigned fgb_shown(void) { return 0; }
void fgb_set_fg(unsigned n) { (void)n; }
static unsigned s_limit_us = ~0u;   // the frame limit the glue handed over (NVIDIA Reflex, us per shown frame)
void fgb_set_frame_limit(unsigned us) { s_limit_us = us; }
static int s_panel_motion;   // the motion row as handed over (-2: needs frame generation)
int fgp_init(ID3D11Device *dev, ID3D11DeviceContext *ctx, UINT bw, UINT bh, DXGI_FORMAT fmt, int sr, int fg, int motion, FgpApply apply) {
    s_panel_motion = motion;
    (void)dev; (void)ctx; (void)fmt; (void)fg;
    s_panel_init = bw == W && bh == H && sr == 1 && motion == 1 && apply.sr && apply.fg && apply.motion && !apply.view == !DEV_VIEW;   // the debug view switch: dev builds only
    return 1;
}
void fgp_frame(ID3D11Resource *bb, HWND hwnd) { (void)bb; (void)hwnd; s_panel_frames++; }
void fgp_set_fps(double game, double shown) { (void)game; (void)shown; }
void fgp_set_sr_choices(int n) { (void)n; }
static int s_panel_cap = -1;
static unsigned s_panel_cap_fps;
void fgp_set_cap(int choice, unsigned fps) { s_panel_cap = choice; s_panel_cap_fps = fps; }
void fgp_di8_created(void *di) { (void)di; }
static int s_panel_moved;
static const wchar_t *s_panel_note;
int fgp_move_device(ID3D11Device *dev, ID3D11DeviceContext *ctx, UINT bw, UINT bh) { (void)dev; (void)ctx; s_panel_moved += bw == W && bh == H; return 1; }
void fgp_set_note(const wchar_t *note) { s_panel_note = note; }
void fgb_jitter(float *jx, float *jy) { *jx = s_sr ? 0.25f : 0; *jy = s_sr ? -0.375f : 0; }
void fgb_sr_stats(unsigned *frames, unsigned *timeouts, double *wait_avg_ms, double *wait_max_ms) { *frames = *timeouts = 0; *wait_avg_ms = *wait_max_ms = 0; }
// the Rome 2-like model (2 bones, 88-byte record): a quad at (r2_x, 0, 1.5) moving 0.012 a frame while the camera pans
// 0.01 the other way; its motion at its centre from this frame's and the last frame's camera and place, in render pixels
static float r2_x(int fr) { return -0.5f + 0.012f * fr; }
static void r2_ndc(int fr, float *n) {
    float p[4] = {r2_x(fr), 0, 1.5f, 1}, c[4] = {0};
    for (int r = 0; r < 4; r++) for (int k = 0; k < 4; k++) c[r] += main_vp(fr, r * 4 + k) * p[k];
    n[0] = c[0] / c[3]; n[1] = c[1] / c[3];
}
static float h2f(unsigned short h) {
    unsigned e = (h >> 10) & 31, m = h & 1023;
    float v = e ? ldexpf((float)(m | 1024), (int)e - 25) : ldexpf((float)m, -24);
    return h >> 15 ? -v : v;
}
static int r2_check(void) {
    float c[2], p[2], jx, jy;
    r2_ndc(g_fr, c);
    r2_ndc(g_fr - 1, p);
    fgb_jitter(&jx, &jy);
    int px = (int)floorf((c[0] + 1) * 0.5f * s_rw + jx), py = (int)floorf((1 - c[1]) * 0.5f * s_rh + jy), moved = 0;
    float ex = (p[0] - c[0]) * 0.5f * s_rw, ey = (p[1] - c[1]) * -0.5f * s_rh, gx = 0, gy = 0;
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)s_mv_st, (ID3D11Resource *)s_mv);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)s_mv_st, 0, D3D11_MAP_READ, 0, &m))) {
        for (UINT y = 0; y < H; y++)
            for (UINT x = 0; x < W; x++) moved += ((const unsigned *)((const char *)m.pData + y * m.RowPitch))[x] != 0;
        const unsigned short *q = (const unsigned short *)((const char *)m.pData + py * m.RowPitch) + px * 2;
        gx = h2f(q[0]); gy = h2f(q[1]);
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)s_mv_st, 0);
    }
    printf("  object motion at frame %d (render %ux%u): pixel %d,%d: %.4f %.4f, expected %.4f %.4f; %d pixels moved\n", g_fr, s_rw, s_rh,
           px, py, gx, gy, ex, ey, moved);
    return fabsf(gx - ex) < 0.01f && fabsf(gy - ey) < 0.01f && moved >= 90;   // the whole quad: 21 x 21 native, 10 x 10 at half size
}
// the motion vector view (F11 panel): the frame handed over is untouched (scene blue or UI red) except the moving model, tinted
static int view_check(ID3D11Resource *bb) {
    float c[2], jx, jy;
    r2_ndc(g_fr, c);
    fgb_jitter(&jx, &jy);
    int px = (int)floorf((c[0] + 1) * 0.5f * W + jx), py = (int)floorf((1 - c[1]) * 0.5f * H + jy);
    unsigned model = 0, corner = 0;
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)s_stage, bb);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)s_stage, 0, D3D11_MAP_READ, 0, &m))) {
        model = ((const unsigned *)((const char *)m.pData + py * m.RowPitch))[px];
        corner = ((const unsigned *)m.pData)[2];
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)s_stage, 0);
    }
    printf("  motion vector view at frame %d: model pixel %d,%d %08x, corner %08x (untinted: blue ffff0000 or UI red ff0000ff)\n", g_fr, px, py, model, corner);
    return model != corner && (model & 0xFFFF) && (corner == 0xFFFF0000u || corner == 0xFF0000FFu);   // the corner: the scene or the UI, untinted
}
// DLAA / presets: the viewport applied right now must be jittered (by the stub's 0.25, -0.375) or not, and shrunk to the
// render size or not (scaled), and the game must read back its own
static int s_jit_bad;
static void jit_check2(ID3D11DeviceContext *ctx, int want, int scaled, const char *where) {
    D3D11_VIEWPORT got;
    UINT n = 1;
    ID3D11DeviceContext_RSGetViewports(ctx, &n, &got);
    float ew = scaled ? (float)s_rw : got.Width, eh = scaled ? (float)s_rh : got.Height;   // shrunk, or as the game set it
    int ok = got.TopLeftX == 0 && got.TopLeftY == 0 &&
             (want ? g_avp[0].TopLeftX == 0.25f && g_avp[0].TopLeftY == -0.375f : g_avp[0].TopLeftX == 0 && g_avp[0].TopLeftY == 0) &&
             g_avp[0].Width == ew && g_avp[0].Height == eh;
    if (!ok && s_jit_bad++ < 3)
        printf("  viewport at frame %d, %s: applied (%g, %g) %gx%g, read back (%g, %g) %gx%g, jitter expected %d, scaled %d\n", g_fr, where,
               g_avp[0].TopLeftX, g_avp[0].TopLeftY, g_avp[0].Width, g_avp[0].Height, got.TopLeftX, got.TopLeftY, got.Width, got.Height,
               want, scaled);
}
static void jit_check(ID3D11DeviceContext *ctx, int want, const char *where) { jit_check2(ctx, want, 0, where); }
// below native size the pixel shaders' camera block: a world point drawn at render size in the top-left, turned back
// from its pixel with the full screen size (as the game's shaders do), must be the same point; and the other way round
static int inv4(const float *m, float *out) {
    float a[4][8];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 8; c++) a[r][c] = c < 4 ? m[r * 4 + c] : (c - 4 == r);
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++) if (fabsf(a[r][c]) > fabsf(a[p][c])) p = r;
        if (fabsf(a[p][c]) < 1e-9f) return 0;
        for (int k = 0; k < 8; k++) { float t = a[c][k]; a[c][k] = a[p][k]; a[p][k] = t; }
        float d = a[c][c];
        for (int k = 0; k < 8; k++) a[c][k] /= d;
        for (int r = 0; r < 4; r++) if (r != c) { float f = a[r][c]; for (int k = 0; k < 8; k++) a[r][k] -= f * a[c][k]; }
    }
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) out[r * 4 + c] = a[r][c + 4];
    return 1;
}
static void mul4(const float *m, const float *v, float *o) { for (int r = 0; r < 4; r++) o[r] = m[r * 4] * v[0] + m[r * 4 + 1] * v[1] + m[r * 4 + 2] * v[2] + m[r * 4 + 3] * v[3]; }
static int cam_patch_check(int bottom) {   // bottom: the scene in the bottom-left corner (the ocean's reflection)
    float f[144] = {0}, mv[16], inv[16];
    for (int i = 0; i < 16; i++) mv[i] = main_vp(7, i);
    if (!inv4(mv, inv)) return 0;
    memcpy(f + 20, mv, 64); memcpy(f + 36, mv, 64);   // projection, view_projection
    memcpy(f + 68, inv, 64); memcpy(f + 84, inv, 64);  // inverse_projection, inv_view_projection
    UINT rw = W / 2, rh = H / 2;
    cam_patch(f, rw, rh, bottom);
    const float pw[4] = {0.3f, -0.2f, 10, 1};
    float c[4], o[4], back[4];
    mul4(mv, pw, c);
    float nx = c[0] / c[3], ny = c[1] / c[3], nz = c[2] / c[3];
    float px = (nx * 0.5f + 0.5f) * rw, py = (0.5f - ny * 0.5f) * rh + (bottom ? H - rh : 0);   // drawn at (shrunk viewport)
    float fx = px / W * 2 - 1, fy = 1 - py / H * 2;                     // what the shader makes of it (full screen size)
    mul4(f + 36, pw, o);   // forward: the point to the "full-size" ndc
    const float q[4] = {fx, fy, nz, 1};
    mul4(f + 84, q, back);  // inverse: that ndc back to the world
    int ok = fabsf(o[0] / o[3] - fx) < 1e-4f && fabsf(o[1] / o[3] - fy) < 1e-4f;
    for (int i = 0; i < 3; i++) ok &= fabsf(back[i] / back[3] - pw[i]) < 1e-3f;
    if (!ok) printf("  camera copy: ndc (%g %g) expected (%g %g), world back (%g %g %g)\n", o[0] / o[3], o[1] / o[3], fx, fy,
                    back[0] / back[3], back[1] / back[3], back[2] / back[3]);
    return ok;
}
#else
void fgp_test_open(int open);   // fgpanel.c: the F11 panel open without a key
#endif

int main(int argc, char **argv) {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    swprintf(dir + wcslen(dir), MAX_PATH - wcslen(dir), L"bridgelive_test_%lu", GetCurrentProcessId());
    CreateDirectoryW(dir, NULL);
    wchar_t p[MAX_PATH];
    swprintf(p, MAX_PATH, L"%ls\\fgbridge_on.txt", dir);
    // perf [off]: uncapped frames with a heavy scene shader, frame rate measured, bridge on or off (the in-game A/B:
    // 65 fps with frame generation 2x vs 194 without, dlss/DLSS_LOG.md 11)
    int perf = 0, off = 0;
    static char iters[16] = "30000";   // the scene shader's loop (GPU time per frame)
#ifdef REAL_BRIDGE
    if (argc < 3) { printf("usage: bridgelive_test <helper64.exe> <streamline dir> [perf [off] [4k] [iters=N]]\n"); return 1; }
    perf = argc > 3 && !strcmp(argv[3], "perf");
    for (int i = 4; perf && i < argc; i++) {
        if (!strcmp(argv[i], "off")) off = 1;
        if (!strcmp(argv[i], "4k")) {   // the real screen size, not the DPI-scaled one (like the game)
            SetProcessDPIAware();
            W = (unsigned)GetSystemMetrics(SM_CXSCREEN);
            H = (unsigned)GetSystemMetrics(SM_CYSCREEN);
        }
        if (!strncmp(argv[i], "iters=", 6)) snprintf(iters, sizeof iters, "%d", atoi(argv[i] + 6));
    }
    if (!off) {
        const char *mode = "", *cap = "";   // dlaa / quality / balanced / perf / ultraperf: that DLSS mode; capoff / cap60 ...: the frame cap
        static const char *const modes[5] = {"dlaa", "quality", "balanced", "perf", "ultraperf"};
        for (int i = 3; i < argc; i++) {
            for (int k = 0; k < 5; k++) if (!strcmp(argv[i], modes[k])) mode = modes[k];
            if (!strncmp(argv[i], "cap", 3)) cap = argv[i];
        }
        FILE *on = _wfopen(p, L"w");
        fprintf(on, "%s\n%s\n1\n%s%s%s\n", argv[1], argv[2], mode, mode[0] && cap[0] ? " " : "", cap);
        fclose(on);
    }
#else
    int nofg = argc > 1 && !strcmp(argv[1], "nofg");
    s_fg_max = !nofg;
    if (argc > 1 && !strcmp(argv[1], "addon")) {   // the release add-on: <game>\FrameBridge\helper64.exe + settings.txt, no fgbridge_on.txt
        wchar_t fb[MAX_PATH], q[MAX_PATH];
        swprintf(fb, MAX_PATH, L"%ls\\FrameBridge", dir);
        CreateDirectoryW(fb, NULL);
        swprintf(q, MAX_PATH, L"%ls\\helper64.exe", fb);
        FILE *h = _wfopen(q, L"w");
        fclose(h);
        swprintf(q, MAX_PATH, L"%ls\\settings.txt", fb);
        DeleteFileW(q);
        DeleteFileW(p);   // a dev run's fgbridge_on.txt left in a reused folder (same process id) would win over the defaults
        // a first start (no settings.txt): Quality, frame generation 2x, camera-only motion, and the file written with them
        CHECK(install_bridgeprobe(GetModuleHandleW(NULL), dir, tlog) && g_live && g_sr == 2 && g_fg == 1 && !g_motion_on, "add-on defaults: live %d, SR %d, FG %u, motion %d",
              g_live, g_sr, g_fg, g_motion_on);
        CHECK(!wcscmp(g_logdir, fb) && !wcscmp(g_settings, q) && wcsstr(g_helper, L"FrameBridge\\helper64.exe") && !wcscmp(g_sl, fb),
              "add-on paths: logs %ls, settings %ls, helper %ls, Streamline %ls", g_logdir, g_settings, g_helper, g_sl);
        char text[1024] = {0};
        FILE *sf = _wfopen(q, L"r");
        CHECK(sf, "settings.txt not written at the first start");
        fread(text, 1, sizeof text - 1, sf);
        fclose(sf);
        CHECK(strstr(text, "\nsuper_resolution=quality\n") && strstr(text, "\nframe_generation=2x\n") && strstr(text, "\nmotion_vectors=camera_only\n") &&
              strstr(text, "\nframe_cap=auto\n") && strstr(text, "addon=on") && strstr(text, "# super_resolution:"), "settings.txt at the first start:\n%s", text);
        // what the F11 panel saves, read back the same; a player's own file with other values
        g_sr = 4; g_fg = 0; g_motion_on = 0; g_fcap = 4;
        settings_save();
        memset(text, 0, sizeof text);
        sf = _wfopen(q, L"r");
        fread(text, 1, sizeof text - 1, sf);
        fclose(sf);
        static const char *const MV[2] = {"camera_only", "per_object"};
        CHECK(settings_value(text, "super_resolution=", SR_KEYS, 6, 1) == 4 && settings_value(text, "frame_generation=", FG_KEYS, 4, 1) == 0 &&
              settings_value(text, "motion_vectors=", MV, 2, 1) == 0 && settings_value(text, "frame_cap=", CAP_KEYS, 5, 1) == 4,
              "settings.txt as saved:\n%s", text);
        const char *own = "# a comment\nsuper_resolution=balanced\nframe_generation=3x\nmotion_vectors=per_object\nframe_cap=90\naddon=on\n";
        CHECK(settings_value(own, "super_resolution=", SR_KEYS, 6, 2) == 3 && settings_value(own, "frame_generation=", FG_KEYS, 4, 1) == 2 &&
              settings_value(own, "motion_vectors=", MV, 2, 0) == 1 && settings_value(own, "frame_cap=", CAP_KEYS, 5, 1) == 3 &&
              settings_value("frame_cap=off\n", "frame_cap=", CAP_KEYS, 5, 1) == 0 && settings_value("frame_cap=144\n", "frame_cap=", CAP_KEYS, 5, 1) == 1,
              "a player's settings.txt");
        // the dev switch file's words (fgbridge_on.txt line 4): whole words only
        CHECK(has_word("dlaa motion cap60", "cap60") && has_word("cap60", "cap60") && !has_word("dlaa cap600", "cap60") &&
              has_word("perf motionprobe motion", "motion") && !has_word("dlaa motionprobe", "motion"), "dev switch words");
        CHECK(settings_value("", "super_resolution=", SR_KEYS, 6, 1) == 1 && settings_value("frame_generation=8x\n", "frame_generation=", FG_KEYS, 4, 1) == 1 &&
              settings_value("super_resolution=performance_x\n", "super_resolution=", SR_KEYS, 6, 1) == 1, "settings fallbacks");
        printf("bridge live test (release add-on settings) passed\n");
        return 0;
    }
    FILE *on = _wfopen(p, L"w");
    fprintf(on, "helper64.exe\nstreamline\n1\ndlaa motion motionprobe0\n");   // motion: the motion pass on; motionprobe0: the probe captures at once
    fclose(on);
#endif
    CHECK(install_bridgeprobe(GetModuleHandleW(NULL), dir, tlog) && (g_live || off), "install / fgbridge_on.txt");

    HMODULE d3d = LoadLibraryW(L"d3d11.dll"), dxgi = LoadLibraryW(L"dxgi.dll");
    create_fn create = (create_fn)(void *)GetProcAddress(d3d, "D3D11CreateDevice");
    factory_fn factory = (factory_fn)(void *)GetProcAddress(dxgi, "CreateDXGIFactory1");
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    // Rome 2 creates its device SINGLETHREADED (flags 0x1 in its log); perf "mt" leaves that out
    int mt = 0, rb = 0;   // rb: perf "readback=K": each frame a small GPU copy read on the CPU K frames later (games do; Rome 2's tile feedback)
    double cpu_ms = 0;    // perf "cpu=<ms>": the game's own CPU work per frame (busy loop before the frame's draws)
    for (int i = 4; perf && i < argc; i++) {
        mt |= !strcmp(argv[i], "mt");
        if (!strncmp(argv[i], "readback=", 9)) rb = atoi(argv[i] + 9);
        if (!strncmp(argv[i], "cpu=", 4)) cpu_ms = atof(argv[i] + 4);
    }
    CHECK(SUCCEEDED(create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, mt ? 0 : D3D11_CREATE_DEVICE_SINGLETHREADED, NULL, 0, D3D11_SDK_VERSION, &dev, NULL, &ctx)), "device");
    IDXGIFactory *fac;
    CHECK(SUCCEEDED(factory(&iid_factory1, (void **)&fac)), "factory");
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = L"bltest";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, L"bltest", L"bridgelive_test", WS_POPUP | WS_VISIBLE, 0, 0, W, H, NULL, NULL, wc.hInstance, NULL);
    SetForegroundWindow(hwnd);
    // like Rome 2: a blit swapchain, one buffer; asked windowed here (exclusive fullscreen is asked below through SetFullscreenState)
    DXGI_SWAP_CHAIN_DESC scd = {{W, H, {0, 1}, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 0}, {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT, 1,
                                hwnd, TRUE, DXGI_SWAP_EFFECT_DISCARD, DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH};
    IDXGISwapChain *sc;
    CHECK(SUCCEEDED(IDXGIFactory_CreateSwapChain(fac, (IUnknown *)dev, &scd, &sc)), "swapchain");
    CHECK(off || (g_live_state == 1 && g_hid_sc == sc), "bridge not started (state %d)", g_live_state);
    DXGI_SWAP_CHAIN_DESC got;
    if (off) IDXGISwapChain_GetDesc(sc, &got);
    else r_getdesc(sc, &got);   // the real one (the hook says the game's window, below)
    CHECK(off || (got.OutputWindow != hwnd && got.Windowed && g_out_hwnd == hwnd), "the game's swapchain is not on the hidden window");
    if (!off) {   // what other mods see: the game's window, through GetDesc and IDXGISwapChain1::GetHwnd
        static const GUID iid_sc1 = {0x790a45f7, 0x0d42, 0x4876, {0x98, 0x3a, 0x0a, 0x55, 0xcf, 0xe6, 0xf4, 0xaa}};
        IDXGISwapChain1 *s1 = NULL;
        HWND gh = NULL;
        DXGI_SWAP_CHAIN_DESC seen;
        IDXGISwapChain_GetDesc(sc, &seen);
        if (SUCCEEDED(IDXGISwapChain_QueryInterface(sc, &iid_sc1, (void **)&s1))) { IDXGISwapChain1_GetHwnd(s1, &gh); IDXGISwapChain1_Release(s1); }
        CHECK(seen.OutputWindow == hwnd && gh == hwnd, "other mods asking the swapchain for its window: GetDesc %p, GetHwnd %p (game's %p)",
              (void *)seen.OutputWindow, (void *)gh, (void *)hwnd);
    }
#ifndef REAL_BRIDGE
    CHECK(s_started == 1 && s_hwnd == hwnd, "the helper is not started once, presenting into the game's window");
    if (nofg) {   // no frame generation: no motion pass set up, its panel row greyed "Needs frame generation"
        CHECK(!g_motion && s_panel_motion == -2 && s_limit_us == 0, "no frame generation: motion pass %d, panel row %d, frame limit %u us",
              g_motion, s_panel_motion, s_limit_us);
        printf("bridge live test (no frame generation) passed\n");
        return 0;
    }
    CHECK(cam_patch_check(0) && cam_patch_check(1), "the pixel shaders' camera copy below native size (top-left, bottom-left)");
    // the frame cap (NVIDIA Reflex limit, us per shown frame, rounded up so never above the screen's rate): Auto at 2x =
    // the screen's rate / 2 rendered; Auto without frame generation: none; 60: 60 rendered, 60 or 120 shown
    unsigned hz = refresh_hz(hwnd), want = hz ? (unsigned)ceil(1e6 / (hz / 2 * 2.0)) : 0;
    printf("  frame cap at start: %s, screen %u Hz -> %u rendered fps, limit %u us\n", CAP_KEYS[g_fcap], hz, s_panel_cap_fps, s_limit_us);
    CHECK(g_fcap == 1 && g_fg == 1 && s_limit_us == want && s_panel_cap == 1 && s_panel_cap_fps == hz / 2, "frame cap Auto at 2x: limit %u us (want %u)",
          s_limit_us, want);
    panel_fg(0);
    CHECK(s_limit_us == 0 && s_panel_cap_fps == 0, "frame cap Auto without frame generation: %u us", s_limit_us);
    panel_cap(2);
    CHECK(s_limit_us == 16667 && s_panel_cap_fps == 60, "frame cap 60 without frame generation: %u us", s_limit_us);
    panel_fg(1);
    CHECK(s_limit_us == 8334 && s_panel_cap_fps == 60, "frame cap 60 at 2x: %u us", s_limit_us);
    panel_cap(1);
    CHECK(s_limit_us == want && s_panel_cap == 1, "frame cap back to Auto: %u us", s_limit_us);
    BOOL fs = FALSE;
    CHECK(SUCCEEDED(IDXGISwapChain_SetFullscreenState(sc, TRUE, NULL)) && SUCCEEDED(IDXGISwapChain_GetFullscreenState(sc, &fs, NULL)) && fs,
          "fullscreen request not answered as asked");
    IDXGISwapChain_GetDesc(sc, &got);
    CHECK(got.Windowed, "the hidden swapchain went exclusive fullscreen");
#endif

    // targets: back buffer, scene (R16F x4) + its R24G8 depth, the same depth with 4x MSAA, shadow map depth
    ID3D11Texture2D *bb, *scene, *ds, *dsm, *shadow;
    IDXGISwapChain_GetBuffer(sc, 0, &iid_tex2d, (void **)&bb);
    D3D11_TEXTURE2D_DESC td = {W, H, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0};
    ID3D11Device_CreateTexture2D(dev, &td, NULL, &scene);
    td.Format = DXGI_FORMAT_R24G8_TYPELESS; td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Device_CreateTexture2D(dev, &td, NULL, &ds);
    td.SampleDesc.Count = 4; td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &dsm)), "MSAA depth");
    td.SampleDesc.Count = 1; td.Width = td.Height = 64;
    ID3D11Device_CreateTexture2D(dev, &td, NULL, &shadow);
    ID3D11RenderTargetView *bbv, *scv;
    ID3D11DepthStencilView *dsv, *dsmv, *shv, *dsro;   // dsro: the scene depth read-only (Rome 2's fog / lighting passes)
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)bb, NULL, &bbv);
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)scene, NULL, &scv);
    D3D11_DEPTH_STENCIL_VIEW_DESC dvd = {DXGI_FORMAT_D24_UNORM_S8_UINT, D3D11_DSV_DIMENSION_TEXTURE2D, 0, {{0}}};
    ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource *)ds, &dvd, &dsv);
    dvd.Flags = D3D11_DSV_READ_ONLY_DEPTH | D3D11_DSV_READ_ONLY_STENCIL;
    ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource *)ds, &dvd, &dsro);
    dvd.Flags = 0;
    ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource *)shadow, &dvd, &shv);
    dvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
    ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource *)dsm, &dvd, &dsmv);
    D3D11_BUFFER_DESC bd = {560, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    ID3D11Buffer *cam;
    ID3D11Device_CreateBuffer(dev, &bd, NULL, &cam);
    // shaders (draws with nothing bound hung the GPU on the test PC): a full-screen triangle, blue / red / scene pixel shaders
    static const char hlsl[] = "#ifndef ITERS\n#define ITERS 30000\n#endif\n"
                               "float4 vs(uint i : SV_VertexID) : SV_Position { float2 t = float2((i << 1) & 2, i & 2); return float4(t * float2(2, -2) + float2(-1, 1), 0.5, 1); }\n"
                               "float4 blue() : SV_Target { return float4(0, 0, 1, 1); }\n"
                               "float4 red() : SV_Target { return float4(1, 0, 0, 1); }\n"
                               "float4 heavy(float4 p : SV_Position) : SV_Target { float a = p.x * 0.001;\n"
                               "  [loop] for (int i = 0; i < ITERS; i++) a = sin(a) * 1.0001 + 0.1; return float4(0, 0, frac(a), 1); }\n";
    typedef HRESULT(WINAPI * comp_fn)(LPCVOID, SIZE_T, LPCSTR, const void *, void *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);
    comp_fn comp = (comp_fn)(void *)GetProcAddress(LoadLibraryExA("d3dcompiler_47.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32), "D3DCompile");
    ID3DBlob *vsb, *bpb, *rpb, *hpb;
    CHECK(comp && SUCCEEDED(comp(hlsl, sizeof hlsl - 1, "t", NULL, NULL, "vs", "vs_5_0", 0, 0, &vsb, NULL)) &&
          SUCCEEDED(comp(hlsl, sizeof hlsl - 1, "t", NULL, NULL, "blue", "ps_5_0", 0, 0, &bpb, NULL)) &&
          SUCCEEDED(comp(hlsl, sizeof hlsl - 1, "t", NULL, NULL, "red", "ps_5_0", 0, 0, &rpb, NULL)) &&
          SUCCEEDED(comp(hlsl, sizeof hlsl - 1, "t", (const D3D_SHADER_MACRO[]){{"ITERS", iters}, {NULL, NULL}}, NULL, "heavy", "ps_5_0", 0, 0, &hpb, NULL)), "shader compile");
    ID3D11VertexShader *vs;
    ID3D11PixelShader *blue, *red, *heavy;
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &vs);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(bpb), ID3D10Blob_GetBufferSize(bpb), NULL, &blue);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(rpb), ID3D10Blob_GetBufferSize(rpb), NULL, &red);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(hpb), ID3D10Blob_GetBufferSize(hpb), NULL, &heavy);
    D3D11_VIEWPORT vp = {0, 0, W, H, 0, 1}, svp = {0, 0, 64, 64, 0, 1};
#ifndef REAL_BRIDGE
    // animated models the way Rome 2 draws them (dx11 weightedcommon vs30_main: cbuffer variant_animation_size, the bones
    // in s_animation_map, per-instance world rows + bone row in TEXCOORD0..3): instanced, from a dynamic instance buffer
    static const char ahlsl[] =
        "cbuffer camera_VS_PS : register(b0) { float4 pad[9]; row_major float4x4 view_projection; }\n"
        "cbuffer variant_animation_size : register(b1) { float2 g_animation_texture_width; float2 g_animation_texture_height; }\n"
        "Texture2D s_animation_map : register(t0); SamplerState smp : register(s0);\n"
        "float4 vs(float4 p : POSITION0, float4 w0 : TEXCOORD0, float4 w1 : TEXCOORD1, float4 w2 : TEXCOORD2, float4 t3 : TEXCOORD3) : SV_Position {\n"
        "  float4 b = s_animation_map.SampleLevel(smp, float2(g_animation_texture_width.y, g_animation_texture_height.y * t3.x), 0);\n"
        "  float4 q = float4(p.xyz + b.xyz * 0.001, 1); float4 w = float4(dot(q, w0), dot(q, w1), dot(q, w2), 1);\n"
        "  return mul(view_projection, w); }\n";
    ID3DBlob *avb;
    CHECK(SUCCEEDED(comp(ahlsl, sizeof ahlsl - 1, "a", NULL, NULL, "vs", "vs_5_0", 0, 0, &avb, NULL)), "animated shader compile");
    ID3D11VertexShader *avs;
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(avb), ID3D10Blob_GetBufferSize(avb), NULL, &avs);
    D3D11_INPUT_ELEMENT_DESC ae[5] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
                                      {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
                                      {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
                                      {"TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1},
                                      {"TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48, D3D11_INPUT_PER_INSTANCE_DATA, 1}};
    ID3D11InputLayout *ail;
    CHECK(SUCCEEDED(ID3D11Device_CreateInputLayout(dev, ae, 5, ID3D10Blob_GetBufferPointer(avb), ID3D10Blob_GetBufferSize(avb), &ail)), "animated layout");
    float mesh[3][4] = {{0, 0, 5, 1}, {1, 0, 5, 1}, {0, 1, 5, 1}};
    unsigned short aidx[3] = {0, 1, 2};
    D3D11_BUFFER_DESC mbd = {sizeof mesh, D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
    D3D11_SUBRESOURCE_DATA msd = {mesh, 0, 0}, isd2 = {aidx, 0, 0};
    ID3D11Buffer *amesh, *aib, *ainst, *acb;
    ID3D11Device_CreateBuffer(dev, &mbd, &msd, &amesh);
    mbd.ByteWidth = sizeof aidx; mbd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    ID3D11Device_CreateBuffer(dev, &mbd, &isd2, &aib);
    D3D11_BUFFER_DESC ibd2 = {4096, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    ID3D11Device_CreateBuffer(dev, &ibd2, NULL, &ainst);
    D3D11_BUFFER_DESC acbd = {16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    float asz[4] = {1.0f / 768, 1.0f / 768, 1.0f / 512, 1.0f / 512};
    D3D11_SUBRESOURCE_DATA asd = {asz, 0, 0};
    ID3D11Device_CreateBuffer(dev, &acbd, &asd, &acb);
    D3D11_TEXTURE2D_DESC atd = {768, 512, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
    ID3D11Texture2D *atex;
    ID3D11ShaderResourceView *asrv;
    ID3D11Device_CreateTexture2D(dev, &atd, NULL, &atex);
    ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)atex, NULL, &asrv);
    float arec[3][16];   // three instances: world rows (a shift by the instance and the frame), bone row
    // the Rome 2-like model for the motion pass: the pass's own skinning as the "game" shader (2 bones, its checksum given
    // to the glue as an animated main-view shader), mesh position + bone bytes, 88-byte records at a buffer offset
    static const char r2game[] =
        "struct GIn { float4 pos : POSITION0; float4 bi : POSITION1; float4 w0 : TEXCOORD0; float4 w1 : TEXCOORD1; float4 w2 : TEXCOORD2; float4 t3 : TEXCOORD3; };\n"
        "float4 game(GIn g) : SV_Position { VIn i = (VIn)0; i.pos = g.pos; i.bi = g.bi; i.w0 = g.w0; i.w1 = g.w1; i.w2 = g.w2; i.t3 = g.t3;\n"
        "  return place(skin(anim, asz, i, i.t3.x), i.w0, i.w1, i.w2, vp); }\n";
    static char r2src[sizeof r2motion_hlsl + sizeof r2game];
    snprintf(r2src, sizeof r2src, "%s%s", r2motion_hlsl, r2game);
    ID3DBlob *r2b;
    CHECK(SUCCEEDED(comp(r2src, strlen(r2src), "r2game", NULL, NULL, "game", "vs_5_0", 0, 0, &r2b, NULL)), "Rome 2-like shader compile");
    g_anim_sum_test[0] = mp_sum(ID3D10Blob_GetBufferPointer(r2b), ID3D10Blob_GetBufferSize(r2b));
    ID3D11VertexShader *r2vs;
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(r2b), ID3D10Blob_GetBufferSize(r2b), NULL, &r2vs);
    D3D11_INPUT_ELEMENT_DESC r2e[6] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
                                       {"POSITION", 1, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
                                       {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
                                       {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
                                       {"TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1},
                                       {"TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48, D3D11_INPUT_PER_INSTANCE_DATA, 1}};
    ID3D11InputLayout *r2il;
    CHECK(SUCCEEDED(ID3D11Device_CreateInputLayout(dev, r2e, 6, ID3D10Blob_GetBufferPointer(r2b), ID3D10Blob_GetBufferSize(r2b), &r2il)), "Rome 2-like layout");
    unsigned char r2v[4][16] = {{0}};   // a quad 0.2 wide, all on bone 0 (byte .z) with weight 1 (.x)
    for (int k = 0; k < 4; k++) {
        float q[3] = {k & 1 ? 0.1f : -0.1f, k & 2 ? 0.1f : -0.1f, 0};
        memcpy(r2v[k], q, 12);
        r2v[k][12] = 255;
    }
    unsigned short r2i[6] = {0, 2, 1, 2, 3, 1};
    D3D11_BUFFER_DESC r2bd = {sizeof r2v, D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
    D3D11_SUBRESOURCE_DATA r2sd = {r2v, 0, 0};
    ID3D11Buffer *r2mesh, *r2ib, *r2rec, *r2asz;
    ID3D11Device_CreateBuffer(dev, &r2bd, &r2sd, &r2mesh);
    r2bd.ByteWidth = sizeof r2i; r2bd.BindFlags = D3D11_BIND_INDEX_BUFFER; r2sd.pSysMem = r2i;
    ID3D11Device_CreateBuffer(dev, &r2bd, &r2sd, &r2ib);
    float r2sz[4] = {7, 1.0f / 8, 11, 1.0f / 11};   // its variant_animation_size: 12 x 8 texels
    r2bd.ByteWidth = 16; r2bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; r2sd.pSysMem = r2sz;
    ID3D11Device_CreateBuffer(dev, &r2bd, &r2sd, &r2asz);
    D3D11_BUFFER_DESC r2rd = {4 * 88, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    ID3D11Device_CreateBuffer(dev, &r2rd, NULL, &r2rec);
    static float r2at[8][12][4];   // poses: every bone the identity
    for (int r = 0; r < 8; r++) for (int t = 0; t < 12; t++) r2at[r][t][t % 3] = 1;
    D3D11_TEXTURE2D_DESC r2td = {12, 8, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
    D3D11_SUBRESOURCE_DATA r2tsd = {r2at, 12 * 16, 0};
    ID3D11Texture2D *r2tex;
    ID3D11ShaderResourceView *r2srv;
    ID3D11Device_CreateTexture2D(dev, &r2td, &r2tsd, &r2tex);
    ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)r2tex, NULL, &r2srv);
    D3D11_SAMPLER_DESC r2smp = {D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP,
                                0, 1, D3D11_COMPARISON_NEVER, {0, 0, 0, 0}, 0, D3D11_FLOAT32_MAX};
    ID3D11SamplerState *r2pt;
    ID3D11Device_CreateSamplerState(dev, &r2smp, &r2pt);
    // a naval battle: an overlay target drawn without depth before the scene, read by an ocean (its vertex shader names
    // g_swell_uv_scale, its pixel shader samples ui_colour_overlay_texture at t9 as on ultra water quality)
    static const char ohlsl[] =
        "cbuffer ocean : register(b1) { float4 g_swell_uv_scale; }\n"
        "float4 vs(uint i : SV_VertexID) : SV_Position { float2 t = float2((i << 1) & 2, i & 2); return float4(t * float2(2, -2) + float2(-1, 1), 0.5, 1) + g_swell_uv_scale * 1e-6; }\n"
        "Texture2D ui_colour_overlay_texture : register(t9); Texture2D reflection_texture : register(t5); SamplerState smp : register(s0);\n"
        "float4 ps(float4 p : SV_Position) : SV_Target { return ui_colour_overlay_texture.Sample(smp, p.xy / 64) + reflection_texture.Sample(smp, p.xy / 64); }\n";
    ID3DBlob *ovb, *opb;
    CHECK(SUCCEEDED(comp(ohlsl, sizeof ohlsl - 1, "o", NULL, NULL, "vs", "vs_5_0", 0, 0, &ovb, NULL)) &&
          SUCCEEDED(comp(ohlsl, sizeof ohlsl - 1, "o", NULL, NULL, "ps", "ps_5_0", 0, 0, &opb, NULL)), "ocean shader compile");
    ID3D11VertexShader *ovs;
    ID3D11PixelShader *ops;
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(ovb), ID3D10Blob_GetBufferSize(ovb), NULL, &ovs);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(opb), ID3D10Blob_GetBufferSize(opb), NULL, &ops);
    CHECK(overlay_slots(ID3D10Blob_GetBufferPointer(opb), ID3D10Blob_GetBufferSize(opb), "colour_overlay_texture") == 1u << 9 &&
          overlay_slots(ID3D10Blob_GetBufferPointer(opb), ID3D10Blob_GetBufferSize(opb), "reflection_texture") == 1u << 5 &&
          !overlay_slots(ID3D10Blob_GetBufferPointer(bpb), ID3D10Blob_GetBufferSize(bpb), "colour_overlay_texture"), "ocean slots from the pixel shaders' RDEF");
    D3D11_TEXTURE2D_DESC otd = {W, H, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
    ID3D11Texture2D *otex;
    ID3D11RenderTargetView *ortv;
    ID3D11ShaderResourceView *osrv, *nosrv = NULL;
    ID3D11Device_CreateTexture2D(dev, &otd, NULL, &otex);
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)otex, NULL, &ortv);
    ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)otex, NULL, &osrv);
    otd.Width = W / 2; otd.Height = H / 2;   // the water reflection: half size
    ID3D11Texture2D *rtex;
    ID3D11RenderTargetView *rrtv;
    ID3D11ShaderResourceView *rsrv;
    ID3D11Device_CreateTexture2D(dev, &otd, NULL, &rtex);
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)rtex, NULL, &rrtv);
    ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)rtex, NULL, &rsrv);
    D3D11_VIEWPORT hvp = {0, 0, W / 2, H / 2, 0, 1};
    // the game's screen-space reflection pass (helpers.hlsl: cbuffer screen_space_refl_params, scene_tex t0, depth_tex t1)
    static const char shlsl[] =
        "cbuffer screen_space_refl_params : register(b0) { float4 quality_factor; }\n"
        "Texture2D scene_tex : register(t0); Texture2D depth_tex : register(t1); SamplerState smp : register(s0);\n"
        "float4 ps(float4 p : SV_Position) : SV_Target { return scene_tex.Sample(smp, p.xy / 64) * depth_tex.Sample(smp, p.xy / 64).x + quality_factor; }\n";
    ID3DBlob *spb;
    CHECK(SUCCEEDED(comp(shlsl, sizeof shlsl - 1, "s", NULL, NULL, "ps", "ps_5_0", 0, 0, &spb, NULL)), "screen-space reflection shader compile");
    ID3D11PixelShader *ssrps;
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(spb), ID3D10Blob_GetBufferSize(spb), NULL, &ssrps);
    D3D11_SHADER_RESOURCE_VIEW_DESC dsrd = {DXGI_FORMAT_R24_UNORM_X8_TYPELESS, D3D11_SRV_DIMENSION_TEXTURE2D};
    dsrd.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView *dsrv, *ssr_in[2];
    CHECK(SUCCEEDED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)ds, &dsrd, &dsrv)), "depth view");
    ssr_in[0] = osrv; ssr_in[1] = dsrv;
    float ssr_depth = -1;   // the depth corner the glue made, read back once (Performance)
#endif
    D3D11_DEPTH_STENCIL_DESC dsd = {0};   // depth test off
    ID3D11DepthStencilState *nodepth;
    ID3D11Device_CreateDepthStencilState(dev, &dsd, &nodepth);

#ifdef REAL_BRIDGE
    int frames = perf ? 1500 : 600;   // 100 fps like bridgetest: frame generation needs a few seconds of steady frames
    LARGE_INTEGER pt0 = {0};
#else
    int frames = 240;
#endif
#ifndef REAL_BRIDGE
    int dead_at = 200;   // the helper "dies" at frame 200
#else
    int fg_window = 0;
    unsigned depth_br = ~0u;   // the scene depth at the bottom right, at the UI pass of frame 400
#endif
    for (g_fr = 0; g_fr < frames; g_fr++) {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
#ifndef REAL_BRIDGE
        if (g_fr == dead_at) s_active = 0;
#ifdef PROFILING
        if (g_fr == 55 || g_fr == 90) panel_view(g_fr == 55);   // the F11 panel: the motion vector view from 55 to 89 (a fake flash at 80)
#endif
        if (g_fr == 100) panel_sr(0);   // the F11 panel: DLAA off (from the next frame), on again at 150
        if (g_fr == 150) panel_sr(1);
        if (g_fr == 175) panel_sr(4);   // Performance: the scene at half size in the top-left (this frame already: it was jittered)
#else
        if (g_fr == (perf ? frames - 30 : 450) && !off) fgb_set_fg(0);   // the helper reports the 2x phase when it ends
        if (g_fr == 50 && !perf) fgp_test_open(1);   // the F11 panel drawn over the frames from here on
        if (g_fr == 300) QueryPerformanceCounter(&pt0);
        if (g_fr == 300) fg_window = GetForegroundWindow() == hwnd;   // frame generation pauses in a background window
#endif
        if (cpu_ms > 0) {
            LARGE_INTEGER c0, c1, cf;
            QueryPerformanceCounter(&c0);
            QueryPerformanceFrequency(&cf);
            do QueryPerformanceCounter(&c1); while ((c1.QuadPart - c0.QuadPart) * 1000.0 / cf.QuadPart < cpu_ms);
        }
        int msaa = g_fr >= 20 && g_fr < 25;   // frames with the game's MSAA on
        float c[140] = {0};
        D3D11_MAPPED_SUBRESOURCE m;
        ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
        ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cam);
        ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &cam);   // like Rome 2's lighting shaders (camera_VS_PS in cb0)
        // frame start: 4 draws into the back buffer (red), before the scene
        ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &bbv, NULL);
        ID3D11DeviceContext_PSSetShader(ctx, red, NULL, 0);
        for (int i = 0; i < 4; i++) ID3D11DeviceContext_Draw(ctx, 3, 0);
        // shadow pass: its own depth and camera
        ID3D11DeviceContext_RSSetViewports(ctx, 1, &svp);
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, shv);
        for (int i = 0; i < 16; i++) c[36 + i] = -1000.0f - (float)i;
        ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)cam, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
        memcpy(m.pData, c, sizeof c);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)cam, 0);
        ID3D11DeviceContext_Draw(ctx, 3, 0);
#ifndef REAL_BRIDGE
        // not after a frame without a scene; DLAA off through the panel from frame 101 to 150
        int live = g_fr <= dead_at, jit = live && g_fr >= 1 && !msaa && g_fr != 25 && (g_fr <= 100 || g_fr > 150);
        int scaled = jit && g_fr >= 175;
        if (live) jit_check(ctx, 0, "shadow pass");
        // the naval overlay pass: full size once the ocean's draw of frame 0 showed its texture
        ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &ortv, NULL);
        ID3D11DeviceContext_PSSetShader(ctx, red, NULL, 0);
        ID3D11DeviceContext_Draw(ctx, 3, 0);
        if (live && g_fr >= 1) jit_check2(ctx, 0, 0, "naval overlay");
        // the water reflection (half size, no depth): below native size shrunk into the bottom-left corner, with the
        // pixel shaders' camera copy for that corner
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rrtv, NULL);
        ID3D11DeviceContext_RSSetViewports(ctx, 1, &hvp);
        ID3D11DeviceContext_Draw(ctx, 3, 0);
        if (live && g_fr >= 1) {
            ID3D11Buffer *pcb = NULL;
            ID3D11DeviceContext_PSGetConstantBuffers(ctx, 0, 1, &pcb);
            int ok = scaled ? g_avp[0].Width == s_rw / 2 && g_avp[0].Height == s_rh / 2 && g_avp[0].TopLeftY == H / 2 - s_rh / 2 &&
                                  g_avp[0].TopLeftX == 0 && g_nccopy && pcb == g_ccopy[0].bl && g_ccopy[0].bl_valid
                            : g_avp[0].Width == W / 2 && g_avp[0].Height == H / 2 && g_avp[0].TopLeftY == 0 && pcb == cam;
            if (!ok && s_jit_bad++ < 3)
                printf("  reflection at frame %d: applied (%g, %g) %gx%g, pixel shader cb0 %p (bottom-left copy %p), scaled %d\n", g_fr,
                       g_avp[0].TopLeftX, g_avp[0].TopLeftY, g_avp[0].Width, g_avp[0].Height, (void *)pcb, g_nccopy ? (void *)g_ccopy[0].bl : NULL, scaled);
            if (pcb) ID3D11Buffer_Release(pcb);
        }
#endif
        // the scene: swapchain-sized depth, the main camera
        ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &scv, msaa ? NULL : dsv);
        if (msaa) ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, dsmv);   // (an MSAA depth with a 1-sample target would not bind)
        for (int i = 0; i < 16; i++) c[36 + i] = main_vp(g_fr, i);
        c[100] = 1; c[101] = 13000;   // near / far: a battle camera (the motion probe tells battle from campaign by it)
        ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)cam, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
        memcpy(m.pData, c, sizeof c);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)cam, 0);
        ID3D11DeviceContext_ClearDepthStencilView(ctx, msaa ? dsmv : dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
        ID3D11DeviceContext_PSSetShader(ctx, perf ? heavy : blue, NULL, 0);
        if (!msaa) ID3D11DeviceContext_Draw(ctx, 3, 0);
#ifndef REAL_BRIDGE
        if (!msaa) {   // the animated models, then the game's shader again
            for (int k = 0; k < 3; k++) {
                float r[16] = {1, 0, 0, 0.1f * k + 0.01f * g_fr, 0, 1, 0, 0, 0, 0, 1, 0, (float)(7 + k), 0, 0, 0};
                memcpy(arec[k], r, sizeof r);
            }
            D3D11_MAPPED_SUBRESOURCE am;
            ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)ainst, 0, D3D11_MAP_WRITE_DISCARD, 0, &am);
            memcpy((char *)am.pData + 64, arec, sizeof arec);   // at an offset, like the game's rotating buffers
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)ainst, 0);
            ID3D11Buffer *avbs[2] = {amesh, ainst};
            UINT ast[2] = {16, 64}, aof[2] = {0, 64};
            ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 2, avbs, ast, aof);
            ID3D11DeviceContext_IASetIndexBuffer(ctx, aib, DXGI_FORMAT_R16_UINT, 0);
            ID3D11DeviceContext_IASetInputLayout(ctx, ail);
            ID3D11DeviceContext_VSSetShader(ctx, avs, NULL, 0);
            ID3D11DeviceContext_VSSetConstantBuffers(ctx, 1, 1, &acb);
            ID3D11DeviceContext_VSSetShaderResources(ctx, 0, 1, &asrv);
            ID3D11DeviceContext_DrawIndexedInstanced(ctx, 3, 3, 0, 0, 0);
            // the Rome 2-like model, its record after one at the buffer's start (start instance 1)
            float rr[2][22] = {{0}, {1, 0, 0, r2_x(g_fr), 0, 1, 0, 0, 0, 0, 1, 1.5f, 0}};
            ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)r2rec, 0, D3D11_MAP_WRITE_DISCARD, 0, &am);
            memcpy((char *)am.pData + 88, rr, sizeof rr);
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)r2rec, 0);
            ID3D11Buffer *r2vbs[2] = {r2mesh, r2rec}, *r2cbs[3] = {cam, NULL, r2asz};
            UINT r2st[2] = {16, 88}, r2of[2] = {0, 88};
            ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 2, r2vbs, r2st, r2of);
            ID3D11DeviceContext_IASetIndexBuffer(ctx, r2ib, DXGI_FORMAT_R16_UINT, 0);
            ID3D11DeviceContext_IASetInputLayout(ctx, r2il);
            ID3D11DeviceContext_VSSetShader(ctx, r2vs, NULL, 0);
            ID3D11DeviceContext_VSSetConstantBuffers(ctx, 1, 3, r2cbs);
            ID3D11DeviceContext_VSSetShaderResources(ctx, 0, 1, &r2srv);
            ID3D11DeviceContext_VSSetSamplers(ctx, 0, 1, &r2pt);
            ID3D11DeviceContext_DrawIndexedInstanced(ctx, 6, 1, 0, 0, 1);
            ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
            ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
            ID3D11DeviceContext_IASetIndexBuffer(ctx, NULL, DXGI_FORMAT_UNKNOWN, 0);
            // the screen-space reflection into the water reflection (no depth bound: it reads the scene depth), then the
            // scene's targets again for the ocean, as the game does. Below native size it must read the render-size corner
            ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rrtv, NULL);
            ID3D11DeviceContext_RSSetViewports(ctx, 1, &hvp);
            ID3D11DeviceContext_PSSetShader(ctx, ssrps, NULL, 0);
            ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 2, ssr_in);
            unsigned ssr_before = g_ssr_draws;
            ID3D11DeviceContext_Draw(ctx, 3, 0);
            ID3D11ShaderResourceView *ssr_after[2] = {NULL, NULL};
            ID3D11DeviceContext_PSGetShaderResources(ctx, 0, 2, ssr_after);
            int ssr_ok = ssr_after[0] == osrv && ssr_after[1] == dsrv && (g_ssr_draws == ssr_before + 1) == scaled;
            if (scaled && g_crop[1]) {
                D3D11_TEXTURE2D_DESC cd;
                ID3D11Texture2D_GetDesc(g_crop[1], &cd);
                ssr_ok &= cd.Width == (UINT)s_rw && cd.Height == (UINT)s_rh;
                if (ssr_depth < 0) {   // the corner holds the scene's depth (its triangle at 0.5), all of it
                    D3D11_TEXTURE2D_DESC sd2 = cd;
                    sd2.Usage = D3D11_USAGE_STAGING; sd2.BindFlags = 0; sd2.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    ID3D11Texture2D *st2 = NULL;
                    D3D11_MAPPED_SUBRESOURCE sm2;
                    if (SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &sd2, NULL, &st2))) {
                        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st2, (ID3D11Resource *)g_crop[1]);
                        if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st2, 0, D3D11_MAP_READ, 0, &sm2))) {
                            // the far corner one pixel in: the jittered scene may leave the last row uncovered
                            float a = *(const float *)sm2.pData, b = *(const float *)((const char *)sm2.pData + (cd.Height - 2) * sm2.RowPitch + (cd.Width - 2) * 4);
                            ssr_depth = fabsf(a - 0.5f) < 1e-3f && fabsf(b - 0.5f) < 1e-3f ? 0.5f : a;
                            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st2, 0);
                        }
                        ID3D11Texture2D_Release(st2);
                    }
                }
            }
            if (!ssr_ok && s_jit_bad++ < 3)
                printf("  screen-space reflection at frame %d: views put back %d, corner pass run %u (scaled %d)\n", g_fr,
                       ssr_after[0] == osrv && ssr_after[1] == dsrv, g_ssr_draws - ssr_before, scaled);
            for (int i = 0; i < 2; i++) if (ssr_after[i]) ID3D11ShaderResourceView_Release(ssr_after[i]);
            ID3D11ShaderResourceView *ssr_none[2] = {NULL, NULL};
            ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 2, ssr_none);
            ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &scv, dsv);
            ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
            // the ocean, reading the overlay
            ID3D11DeviceContext_VSSetShader(ctx, ovs, NULL, 0);
            ID3D11DeviceContext_PSSetShader(ctx, ops, NULL, 0);
            ID3D11DeviceContext_PSSetShaderResources(ctx, 9, 1, &osrv);
            ID3D11DeviceContext_PSSetShaderResources(ctx, 5, 1, &rsrv);
            ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &r2pt);
            ID3D11DeviceContext_Draw(ctx, 3, 0);
            ID3D11DeviceContext_PSSetShaderResources(ctx, 9, 1, &nosrv);
            ID3D11DeviceContext_PSSetShaderResources(ctx, 5, 1, &nosrv);
            ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
        }
#endif
        ID3D11DeviceContext_PSSetShader(ctx, blue, NULL, 0);
#ifndef REAL_BRIDGE
        if (live) {
            jit_check2(ctx, jit, scaled, "scene");
            ID3D11Buffer *pcb = NULL;   // what pixel shaders see in cb0: below native size the patched copy
            ID3D11DeviceContext_PSGetConstantBuffers(ctx, 0, 1, &pcb);
            if ((scaled ? !(g_nccopy && pcb == g_ccopy[0].ours && g_ccopy[0].valid) : pcb != cam) && s_jit_bad++ < 3)
                printf("  pixel shader cb0 at frame %d: %p (game's %p, copy %p)\n", g_fr, (void *)pcb, (void *)cam, g_nccopy ? (void *)g_ccopy[0].ours : NULL);
            if (pcb) ID3D11Buffer_Release(pcb);
            ID3D11DeviceContext_OMSetDepthStencilState(ctx, nodepth, 0);   // a full-screen pass without depth test
            jit_check2(ctx, 0, scaled, "depth test off");   // shrunk still (whole screen-sized target), not jittered
            ID3D11DeviceContext_OMSetDepthStencilState(ctx, NULL, 0);
            jit_check2(ctx, jit, scaled, "depth test on again");
        }
#endif
        // the water reflection's camera, written with no depth bound
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &scv, NULL);
#ifndef REAL_BRIDGE
        if (live) jit_check2(ctx, 0, scaled, "reflection (no depth)");
#endif
        for (int i = 0; i < 16; i++) c[36 + i] = 5000.0f + (float)i;
        ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)cam, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
        memcpy(m.pData, c, sizeof c);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)cam, 0);
        // a fog-like pass with the scene depth read-only (the last scene depth view bound before the HUD-less point in Rome 2)
        if (!msaa) {
            ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &scv, dsro);
            ID3D11DeviceContext_Draw(ctx, 3, 0);
        }
        // the scene's copy into the back buffer (blue, 5 draws), then the UI (red) on top
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &bbv, NULL);
        for (int i = 0; i < 5; i++) ID3D11DeviceContext_Draw(ctx, 3, 0);
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &bbv, msaa ? NULL : dsv);
#ifdef REAL_BRIDGE
        if (g_fr == 400 && !perf) {   // the depth the UI tests against: below native size put back to full size (bottom right too)
            D3D11_TEXTURE2D_DESC sd = {W, H, 1, 1, DXGI_FORMAT_R24G8_TYPELESS, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
            ID3D11Texture2D *st = NULL;
            D3D11_MAPPED_SUBRESOURCE sm;
            if (SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &sd, NULL, &st))) {
                ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)ds);
                if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &sm))) {
                    depth_br = *(const unsigned *)((const char *)sm.pData + (H - 4) * sm.RowPitch + (W - 4) * 4) & 0xFFFFFF;
                    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
                }
                ID3D11Texture2D_Release(st);
            }
        }
#endif
        ID3D11DeviceContext_PSSetShader(ctx, red, NULL, 0);
        ID3D11DeviceContext_Draw(ctx, 3, 0);
#ifndef REAL_BRIDGE
        if (live) jit_check(ctx, 0, "UI");
#endif
        if (rb) {   // after the UI: copy 64x64 of the scene, read the copy made rb frames ago
            static ID3D11Texture2D *ring[8];
            int k = rb < 7 ? rb : 7;
            if (!ring[0])
                for (int i = 0; i < 8; i++) {
                    D3D11_TEXTURE2D_DESC s = {64, 64, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
                    ID3D11Device_CreateTexture2D(dev, &s, NULL, &ring[i]);
                }
            D3D11_BOX box = {0, 0, 0, 64, 64, 1};
            ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)ring[g_fr % (k + 1)], 0, 0, 0, 0, (ID3D11Resource *)scene, 0, &box);
            if (g_fr >= k && SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)ring[(g_fr - k) % (k + 1)], 0, D3D11_MAP_READ, 0, &m)))
                ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)ring[(g_fr - k) % (k + 1)], 0);
        }
        HRESULT pr = IDXGISwapChain_Present(sc, 0, 0);
        CHECK(SUCCEEDED(pr), "Present 0x%08lx at frame %d", pr, g_fr);
        HRESULT rr = ID3D11Device_GetDeviceRemovedReason(dev);
        CHECK(rr == S_OK, "device removed after frame %d: 0x%08lx", g_fr, rr);
#ifdef REAL_BRIDGE
        if (!perf) Sleep(10);
#endif
    }
#ifdef REAL_BRIDGE
    unsigned sr_frames = 0, sr_timeouts = 0, jit_frames = g_jit_frames;   // before the report resets them
    double sr_avg, sr_max;
    if (g_sr) fgb_sr_stats(&sr_frames, &sr_timeouts, &sr_avg, &sr_max);
#endif
    bp_report();
#ifdef REAL_BRIDGE
    if (perf) {
        LARGE_INTEGER pt1, qf;
        QueryPerformanceCounter(&pt1);
        QueryPerformanceFrequency(&qf);
        printf("  perf, bridge %s: %.1f game frames per second (frames 300..%d, %ux%u, uncapped; window in the foreground: %s)\n",
               off ? "off" : "on (frame generation 2x)", (frames - 300) * (double)qf.QuadPart / (pt1.QuadPart - pt0.QuadPart), frames, W, H,
               fg_window ? "yes" : "no");
        if (!off) fgb_stop();
        return 0;
    }
#endif
#ifndef REAL_BRIDGE
    // HUD-less copies: frames 0..dead_at (the helper is found gone at its Present) except the 5 MSAA ones, each right
    CHECK(!s_hud_bad, "%d bad HUD-less copies", s_hud_bad);
    // ... and the 3 scene frames that started unscaled with DLSS SR on go out native (0, 25 after the MSAA frames, 150 when
    // DLAA came back)
    CHECK(s_hud_n == (unsigned)dead_at - 7 && g_br_native == 3, "HUD-less copies %u, expected %d; native frames %u", s_hud_n, dead_at - 7,
          g_br_native);
    CHECK(g_br_msaa == 5, "MSAA frames seen %u", g_br_msaa);
    // DLAA: frames 1..20 and 26..dead_at start jittered (each after a frame with a scene; 20 is MSAA, so nothing to jitter)
    CHECK(!s_jit_bad && g_jit_frames == 20u + 75 + (unsigned)dead_at - 150, "DLAA: %d bad viewports, %u frames jittered", s_jit_bad, g_jit_frames);
    CHECK(ssr_depth == 0.5f, "screen-space reflection's depth corner: %g (the scene's 0.5 expected)", ssr_depth);
    // the motion probe: three battle frames (it captured at once), each with every kind of chunk, the instance records as
    // drawn (record 0's x shift = 0.01 x the frame), the meshes once, the animated vertex shader's bytecode
    for (int k = 0; k < 3; k++) {
        swprintf(p, MAX_PATH, L"%ls\\motion_battle_%d.bin", dir, k);
        FILE *mf = _wfopen(p, L"rb");
        int nanim = 0;
        for (LONG i = 0; i < g_nmpvs && i < 4096; i++) nanim += g_mpvs[i].anim;
        CHECK(mf, "motion probe: no motion_battle_%d.bin (vertex shaders %ld, animated %d, layouts %ld, bound %p, seen %lu, done %d, far %g)", k,
              g_nmpvs, nanim, g_nmpil, (void *)g_cur_mpvs, g_mp_seen[0], g_mp_done[0], g_cam_block[101]);
        static const char *const want[] = {"FRAM", "LAYT", "DRAW", "INST", "CBV0", "CBV1", "VSRV", "TEXR", "CAMB", "MAPS", "VSUM"};
        int have[11] = {0}, draws = 0, inst_ok = 0;
        unsigned fram = 0, h[2];
        static unsigned char buf[1 << 22];
        while (fread(h, 4, 2, mf) == 2) {
            for (int t = 0; t < 11; t++) have[t] += !memcmp(&h[0], want[t], 4);
            if (h[1] > sizeof buf) { fseek(mf, (long)h[1], SEEK_CUR); continue; }   // the animation texture (6 MB)
            if (fread(buf, 1, h[1], mf) != h[1]) break;
            if (!memcmp(&h[0], "FRAM", 4)) memcpy(&fram, buf, 4);
            if (!memcmp(&h[0], "DRAW", 4)) draws++;
            if (!memcmp(&h[0], "INST", 4)) {   // u64 id, offset, desc (6 u32), then the records
                const float *rec = (const float *)(buf + 32);
                unsigned off;
                memcpy(&off, buf + 8, 4);
                inst_ok = h[1] == 32 + 3 * 64 && off == 64 && fabsf(rec[3] - 0.01f * fram) < 1e-6f && rec[12] == 7 && rec[16 + 3] - rec[3] > 0.099f;
            }
        }
        fclose(mf);
        int all = 1;
        for (int t = 0; t < 11; t++) all &= have[t] > 0;
        CHECK(all && draws == 1 && inst_ok, "motion_battle_%d.bin: chunks %d, draws %d, records %s (frame %u)", k, all, draws, inst_ok ? "ok" : "wrong", fram);
    }
    swprintf(p, MAX_PATH, L"%ls\\motion_battle_mesh.bin", dir);
    FILE *mm = _wfopen(p, L"rb");
    unsigned mh[2], meshes = 0;
    static unsigned char mbuf[65536];
    while (mm && fread(mh, 4, 2, mm) == 2 && mh[1] <= sizeof mbuf && fread(mbuf, 1, mh[1], mm) == mh[1]) meshes += !memcmp(&mh[0], "MESH", 4);
    if (mm) fclose(mm);
    CHECK(meshes == 2, "motion probe: %u meshes written (vertex + index buffer, once)", meshes);
    WIN32_FIND_DATAW fd;
    swprintf(p, MAX_PATH, L"%ls\\motion_vs_*.bin", dir);
    HANDLE fh = FindFirstFileW(p, &fd);
    CHECK(fh != INVALID_HANDLE_VALUE, "motion probe: the animated vertex shader not saved");
    FindClose(fh);
    // the panel every frame: with the bridge, then after it (helper gone) over the mod's frames, moved once, with a note why
    // the motion pass, run by the bridge at the HUD-less point on the glue's recorded draws: the model's motion right
    // at frame 60 (DLAA, jittered) and 180 (below native size)
    CHECK(s_mv_checks == 2 && !s_mv_bad, "object motion: %d checks, %d wrong", s_mv_checks, s_mv_bad);
#ifndef PROFILING   // release form: no debug view (no panel switch, the frame untouched), the flash probe idle
    CHECK(!s_view_ok && !g_fl_frames && !g_fl_flashes, "release form: view %d, flash probe frames %u, flashes %u", s_view_ok, g_fl_frames, g_fl_flashes);
#else
    CHECK(s_view_ok == 1, "motion vector view at frames 60 and 80: %d", s_view_ok);
    // the flash probe: one flash (frame 80's fake jump), each frame one main-camera write (shadow and reflection cameras not
    // with the scene depth), none other than the one taken; frame 88's record: the camera taken, panning
    const FlFrame *f88 = &g_fl[88 & 7];
    CHECK(g_fl_flashes == 1 && g_fl_frames == 35 && !g_fl_multi && f88->frame == 88 && f88->n == 1 && f88->hud && f88->gap == 1 &&
          f88->change > 0 && fl_change(f88->c[0].vp, f88->vp) == 0, "flash probe: flashes %u, frames %u, other cameras %u; frame %u: %u writes, HUD-less %u, gap %u, change %g",
          g_fl_flashes, g_fl_frames, g_fl_multi, f88->frame, f88->n, f88->hud, f88->gap, f88->change);
#endif
    CHECK(s_panel_init && s_panel_frames == frames && s_panel_moved == 1 && s_panel_note && wcsstr(s_panel_note, L"helper closed"),
          "F11 panel: set up %d, drawn in %d frames, moved %d, note %ls", s_panel_init, s_panel_frames, s_panel_moved, s_panel_note ? s_panel_note : L"-");
    // the choices saved for the next start: the last ones, Performance and 2x
    swprintf(p, MAX_PATH, L"%ls\\fgbridge_on.txt", dir);
    FILE *sf = _wfopen(p, L"r");
    char sl[4][64] = {{0}};
    for (int i = 0; sf && i < 4 && fgets(sl[i], sizeof sl[i], sf); i++) sl[i][strcspn(sl[i], "\r\n")] = 0;
    if (sf) fclose(sf);
    CHECK(!strcmp(sl[0], "helper64.exe") && !strcmp(sl[1], "streamline") && !strcmp(sl[2], "1") && !strcmp(sl[3], "perf motion motionprobe0"),
          "saved choices: [%s] [%s] [%s] [%s]", sl[0], sl[1], sl[2], sl[3]);
    // report counters (frames 0..dead_at live): 3 camera writes a frame, the main one with the scene depth except in MSAA frames
    CHECK(g_br_scene_frames == (unsigned)dead_at - 4 && g_br_camw == 3u * (dead_at + 1) && g_br_camw_scene == (unsigned)dead_at - 4,
          "scene frames %u, camera writes %u (%u with the scene depth)", g_br_scene_frames, g_br_camw, g_br_camw_scene);
    CHECK(s_present_n == dead_at, "frames handed to the helper %d, expected %d", s_present_n, dead_at);
    CHECK(s_stopped == 1 && g_live_state == 2, "helper death: stopped %d, state %d", s_stopped, g_live_state);
    CHECK(s_fallback_n == frames - dead_at, "fallback frames %d, expected %d", s_fallback_n, frames - dead_at);
    // a new swapchain after the bridge (settings change) goes to a hidden window too
    scd.OutputWindow = hwnd;
    IDXGISwapChain *sc2;
    CHECK(SUCCEEDED(IDXGIFactory_CreateSwapChain(fac, (IUnknown *)dev, &scd, &sc2)), "second swapchain");
    r_getdesc(sc2, &got);
    CHECK(g_hid_sc == sc2 && got.OutputWindow != hwnd, "the swapchain after the bridge is not on a hidden window");
    IDXGISwapChain_GetDesc(sc2, &got);
    CHECK(got.OutputWindow == hwnd, "the swapchain after the bridge tells other mods %p, not the game's window", (void *)got.OutputWindow);
#else
    CHECK(fgb_active(), "helper gone");
    if (g_sr) {   // DLSS SR: a round trip for every HUD-less copy, none timed out; below native size the depth put back to full size
        printf("  DLSS %s, render %ux%u: %u frames jittered / scaled, %u DLSS round trips, game CPU wait avg %.3f ms max %.3f ms, %u "
               "timeouts, native frames %u, depth back to full size %s\n", SR_NAMES[g_sr], g_rw, g_rh, jit_frames, sr_frames, sr_avg, sr_max,
               sr_timeouts, g_br_native, saw_upscale ? "yes" : "no");
        // below native size the scene depth (0.5 everywhere the scene drew) must be back at full size: the bottom right, outside
        // the shrunk corner, 0.5 and not the cleared 1.0 (the restore wrote through a read-only view before, capture 2026-10-29)
        printf("  depth at the bottom right during the UI: 0x%06X (scene 0x7FFFFF, cleared 0xFFFFFF)\n", depth_br);
        CHECK(g_rw == W || depth_br < 0x900000, "the depth for the UI is not back at full size");
        CHECK(sr_frames == g_br_hud && !sr_timeouts && jit_frames >= g_br_hud && g_br_native == 2 && (g_rw == W || saw_upscale),
              "DLSS round trips");
    }
    // HUD-less copies: every scene frame (not the 5 MSAA ones), except those shown native with DLSS SR (0, and 25 after MSAA)
    CHECK(g_br_hud + g_br_native == (unsigned)frames - 5 && g_br_novp == 0, "HUD-less copies %u (no camera %u)", g_br_hud, g_br_novp);
    fgb_stop();
    // the helper's report of the 2x phase (frames 0..449): frames shown per present and Streamline's errors
    swprintf(p, MAX_PATH, L"%ls\\fgbridge_helper.log", dir);
    FILE *hl = _wfopen(p, L"r");
    char line[1024], *ph = NULL;
    while (hl && fgets(line, sizeof line, hl))
        if ((ph = strstr(line, "[phase end]"))) break;
    if (hl) fclose(hl);
    double per = 0;
    unsigned bad = 1, tagf = 1, presf = 1;
    const char *q = ph ? strstr(ph, "(") : NULL;
    if (q) per = atof(q + 1);
    if (ph && (q = strstr(ph, "bad status "))) bad = (unsigned)atoi(q + 11);
    if (ph && (q = strstr(ph, "tag failures "))) tagf = (unsigned)atoi(q + 13);
    if (ph && (q = strstr(ph, "present failures "))) presf = (unsigned)atoi(q + 17);
    printf("  helper, frame generation 2x: %.2f frames shown per present, bad status %u, tag failures %u, present failures %u\n", per, bad, tagf, presf);
    CHECK(fabs(per - 2.0) < 0.05 && !bad && !tagf && !presf, "frame generation 2x through the glue%s",
          fg_window ? "" : " (the test window was not in the foreground: run it alone, not right after another test window)");
#endif
    printf("all bridge live tests passed\n");
    return 0;
}
