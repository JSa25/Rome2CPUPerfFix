// Profiling-only (bridgeprobe.txt in the game folder): finds the four places the DLSS frame bridge needs (dlss/DLSS_LOG.md,
// Attila's DLSS_BRIDGE_GUIDE.md section 6). Looking only: nothing that is drawn changes.
// Rome 2's renderer code does not match Attila's (no stage driver or set-camera found by shape), so this works at the
// D3D11 level, the guide's fallback:
//  1. Present: CreateDXGIFactory1 (looked up with GetProcAddress, like D3D11CreateDevice) hands out the real factory with
//     CreateSwapChain wrapped; the swapchain's Present is wrapped. Swapchain description and callers go to the log.
//  2./3. When bridge_go.txt appears in the game folder, the next frame (Present to Present) is captured into
//     bridge_capture_<n>.txt: every render-target change, clear and vertex-shader constant buffer 0 bind, with the targets
//     (back buffer or not, size, format, depth buffer), draws per target set and the game's return addresses; plus
//     half-size dumps of the back buffer whenever the game leaves a back-buffer target set it drew into, and at Present
//     -> where the UI starts and which depth buffer is the main one.
//  4. For that frame and 30 more, every write into a camera constant buffer (camera_VS_PS: 548 bytes used, view_projection
//     at 144, same layout as Attila's) goes to bridge_camera_<n>.bin with its frame, buffer and game caller.
// Live frame bridge (fgbridge_on.txt next to bridgeprobe.txt: helper64.exe path, Streamline dir, frames to generate 0..3,
// options; dlss/DLSS_LOG.md 5/6): the game's Present is replaced by the helper's (fgbridge.c), as in Attila, with the
// HUD-less point and the camera found from D3D11 calls: the camera is a camera-buffer write while the swapchain-sized
// scene depth is bound; the HUD-less point is the target change after draws into the back buffer, once the scene depth
// was drawn into this frame.
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <stdlib.h>
#include <math.h>
#include "iathook.h"

static wchar_t g_dir[MAX_PATH];
static void (*g_log)(const char *fmt, ...);
static unsigned g_base, g_size;   // empire.retail.dll
static ID3D11Device *g_dev;
static ID3D11DeviceContext *g_ctx;
static IDXGISwapChain *g_sc;
static int g_creates;

static unsigned g_frame, g_presents, g_present_other_thread, g_sync[5], g_main_tid, g_nested;
static struct { unsigned ret, game_ret, n; } g_callers[8];
static int g_armed, g_capturing, g_captures, g_camera_left;
static unsigned g_dumps, g_sets, g_draws, g_set_draws, g_set_bb, g_cam_writes;
static FILE *g_cap, *g_cam;
static ID3D11Texture2D *g_staging;

static const GUID iid_resource = {0xdc8e63f3, 0xd12b, 0x4952, {0xb4, 0x7b, 0x5e, 0x45, 0x02, 0x6a, 0x86, 0x2d}};
static const GUID iid_device = {0xdb6f6ddb, 0xac77, 0x4e88, {0x82, 0x53, 0x81, 0x9d, 0xf9, 0xbb, 0xf1, 0x40}};

// fgbridge.c (Attila's, unchanged)
int fgb_start(ID3D11Device *, ID3D11DeviceContext *, HWND, UINT, UINT, DXGI_FORMAT, const wchar_t *, const wchar_t *, const wchar_t *,
              unsigned, void (*)(const char *, ...));
void fgb_hudless(ID3D11Resource *backbuffer, ID3D11Resource *depth, const float vp[16]);
void fgb_present(ID3D11Resource *backbuffer);
int fgb_active(void);
unsigned fgb_frames(void), fgb_pace_timeouts(void), fgb_fg_max(void);
void fgb_stop(void);
IDXGISwapChain *fgb_fallback_frame(ID3D11Resource *src, HRESULT(STDMETHODCALLTYPE *create)(IDXGIFactory *, IUnknown *, DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **));
void fgb_fallback_close(void);
void *fgb_vt_original(void **vt, size_t slot);
void *fgb_unhooked(void *fn);
void fgb_set_test_not_nvidia(int on);
void fgb_cpu_stats(double *hud_ms, double *present_ms, double *pace_ms, unsigned *frames);
void fgb_set_sr(int on, UINT render_w, UINT render_h);
void fgb_jitter(float *jx, float *jy);
void fgb_set_jitter(float jx, float jy);
void fgb_jitter_at(int d, float *jx, float *jy);
void fgb_sr_stats(unsigned *frames, unsigned *timeouts, double *wait_avg_ms, double *wait_max_ms);
static void jit_apply(ID3D11DeviceContext *c, int force), jit_frame_end(void), cam_copy_write(ID3D11Resource *r, const void *src);
static int is_cam_copy(ID3D11Buffer *b);
unsigned fgb_shown(void);
void fgb_set_fg(unsigned frames_to_generate);
void fgb_set_frame_limit(unsigned us);
void fgb_set_preset(unsigned preset);
int fgb_helper_has_presets(void);
void fgb_set_sharpness(float con);
// the F11 panel (fgpanel.c, Attila's; Rome 2: Super Resolution off / DLAA only so far)
typedef struct { void (*sr)(int mode); void (*fg)(int frames); void (*motion)(int on); void (*view)(int on); void (*cap)(int choice); void (*preset)(int choice); void (*jtest)(int mode); void (*sharp)(int choice); } FgpApply;
int fgp_init(ID3D11Device *, ID3D11DeviceContext *, UINT bw, UINT bh, DXGI_FORMAT fmt, int sr, int fg, int motion, FgpApply apply);
void fgp_frame(ID3D11Resource *backbuffer, HWND hwnd);
void fgp_set_fps(double game, double shown);
void fgp_set_sr_choices(int n);
void fgp_set_cap(int choice, unsigned fps);
void fgp_set_preset(int choice);
void fgp_set_sharp(int choice);
void fgp_set_presets_available(int on);
void fgp_di8_created(void *di);
int fgp_move_device(ID3D11Device *dev, ID3D11DeviceContext *ctx, UINT bw, UINT bh);
void fgp_set_note(const wchar_t *note);
extern void (*g_dev_di8_created)(void *di);   // proxy.c: the game's DirectInput (the panel hides its mouse buttons while over it)
// per-object motion (r2motion.c, dlss/DLSS_LOG.md 19-23)
extern volatile int r2m_internal;
int r2m_init(ID3D11Device *, ID3D11DeviceContext *, ID3D11Texture2D *mv, void (*log)(const char *fmt, ...));
void r2m_set_enabled(int on);
void r2m_set_view(int on);   // dev builds only
void r2m_draw(ID3D11DeviceContext *c, int type, const void *game_il, const D3D11_INPUT_ELEMENT_DESC *e, UINT n, int indexed, UINT count,
              UINT inst, UINT start, INT base, UINT sinst);
void r2m_pass(ID3D11DepthStencilView *dsv, const float *cam, UINT rw, UINT rh, float jx, float jy, float min_depth, float max_depth);
void r2m_frame_end(void);
void r2m_report(void (*log)(const char *fmt, ...));
void r2m_show(ID3D11Resource *bb, UINT rw, UINT rh);   // dev builds only
ID3D11Texture2D *fgb_mv_texture(void);
void fgb_set_motion(void (*pass)(ID3D11Resource *depth, float jx, float jy));
static int g_motion, g_motion_on, g_view;   // the motion pass is set up / switched on (fgbridge_on.txt "motion", F11 panel); its debug view
unsigned long long g_anim_sum_test[2];   // tests: their own animated shaders' checksums (2 bones, 4 bones)
// live bridge state: 0 not started, 1 running, 2 off for the session (the game presents through the fallback)
static int g_live, g_live_state;
static wchar_t g_helper[MAX_PATH], g_sl[MAX_PATH];
static char g_on_lines[2][MAX_PATH], g_on_test[32];   // fgbridge_on.txt's paths and test options, kept for settings_save
static wchar_t g_settings[MAX_PATH];   // the release add-on's FrameBridge\settings.txt (the F11 panel saves to it); empty in dev runs
static wchar_t g_logdir[MAX_PATH];     // where the helper writes its logs: FrameBridge (release add-on) or the game folder
static unsigned g_fg;
static UINT g_live_w, g_live_h;
static IDXGISwapChain *g_hid_sc;   // the game's swapchain on the hidden window
static HWND g_out_hwnd;             // the game's window (the helper presents into it)
static float g_vp[16];              // this frame's main camera view-projection (camera_VS_PS @144)
static unsigned g_vp_frame = ~0u, g_scene_frame = ~0u, g_hud_frame = ~0u;
static int g_lv_bb, g_lv_scene;     // the bound target set: back buffer as target 0 / swapchain-sized scene depth
static UINT g_rt_w, g_rt_h;         // its size (render target 0, else the depth buffer)
static int g_rt_overlay;            // target 0 is an ocean input (below): 1 overlay, no depth (full size), 2 reflection (bottom-left)
static ID3D11Resource *g_ovres[8];   // the ocean's overlay / reflection textures (one reference held), oldest replaced
static int g_ovkind[8];              // 1 overlay, 2 reflection
static unsigned g_novres, g_ocean_frame = ~0u;   // textures found so far; the frame of the last ocean draw
static unsigned g_lv_draws;         // draws since the last target change
static ID3D11Resource *g_scene_depth;   // one reference held
static ID3D11DepthStencilView *g_scene_dsv;   // our own writable view of it (for the depth pass below native size)
static int g_sr;                    // DLSS SR: 0 off, 1 DLAA, 2..5 Quality..Ultra Performance (fgbridge_on.txt line 4, F11 panel)
static UINT g_rw, g_rh;             // render size (the screen with DLAA or off)
static int g_frame_scaled;          // this frame started jittered / scaled (its HUD-less copy goes through DLSS)
static unsigned g_br_native;        // scene frames shown native (not scaled: the first after a loading screen)
static unsigned g_br_hud, g_br_novp, g_br_msaa, g_fb_frames;
static unsigned g_br_camw, g_br_camw_scene, g_br_scene_frames;   // camera writes seen / while the scene depth was bound; scene frames
// time the game spends inside Map / UpdateSubresource while the bridge runs (in game the game's own work per frame took ~13 ms
// with the bridge vs ~5 without, dlss/DLSS_LOG.md 11; guess: the driver stalls these behind the bridge's GPU wait)
static LONGLONG g_t_map, g_t_map_max, g_t_upd, g_t_upd_max, g_t_frame_max, g_t_last_present;
static unsigned g_n_map, g_n_upd, g_n_frames;

static int in_game(unsigned a) { return a >= g_base + 0x1000 && a < g_base + g_size; }

// up to n return-address-looking values inside empire.retail.dll on the stack above sp (the first is the real caller;
// later ones may be stale values, they only help to find the calling function in Ghidra)
static int game_callers(const unsigned *sp, unsigned *out, int n) {
    int k = 0;
    for (int i = 0; i < 512 && k < n; i++) {
        MEMORY_BASIC_INFORMATION m;
        if (i % 64 == 0 && (!VirtualQuery(sp + i, &m, sizeof m) || m.State != MEM_COMMIT)) break;
        if (in_game(sp[i])) out[k++] = sp[i] - g_base;
    }
    return k;
}
#define CALLERS(buf)                                                                                         \
    unsigned cs_[3] = {0};                                                                                   \
    int ncs_ = game_callers((const unsigned *)__builtin_frame_address(0), cs_, 3);                          \
    char buf[64];                                                                                            \
    snprintf(buf, sizeof buf, "+0x%X +0x%X +0x%X", ncs_ > 0 ? cs_[0] : 0, ncs_ > 1 ? cs_[1] : 0, ncs_ > 2 ? cs_[2] : 0)

static ID3D11Resource *backbuffer(void) {
    ID3D11Resource *bb = NULL;
    if (g_sc) IDXGISwapChain_GetBuffer(g_sc, 0, &iid_resource, (void **)&bb);
    return bb;
}

static void desc_of(ID3D11View *v, char *out, size_t len, ID3D11Resource *bb) {
    if (!v) { snprintf(out, len, "-"); return; }
    ID3D11Resource *r;
    ID3D11View_GetResource(v, &r);
    D3D11_RESOURCE_DIMENSION dim;
    ID3D11Resource_GetType(r, &dim);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC d;
        ID3D11Texture2D_GetDesc((ID3D11Texture2D *)r, &d);
        snprintf(out, len, "%s%ux%u f%u b%x a%u m%u s%u @%p", r == bb ? "BACKBUFFER " : "", d.Width, d.Height, d.Format, d.BindFlags,
                 d.ArraySize, d.MipLevels, d.SampleDesc.Count, (void *)r);
    } else snprintf(out, len, "dim%d @%p", dim, (void *)r);
    ID3D11Resource_Release(r);
}

// the targets bound right now; returns 1 if the back buffer is render target 0
static int targets(char *out, size_t len) {
    ID3D11RenderTargetView *rtv[2] = {0};
    ID3D11DepthStencilView *dsv = NULL;
    ID3D11DeviceContext_OMGetRenderTargets(g_ctx, 2, rtv, &dsv);
    ID3D11Resource *bb = backbuffer(), *r0 = NULL;
    char a[160], b[160], c[160];
    desc_of((ID3D11View *)rtv[0], a, sizeof a, bb);
    desc_of((ID3D11View *)rtv[1], b, sizeof b, bb);
    desc_of((ID3D11View *)dsv, c, sizeof c, bb);
    snprintf(out, len, "rt0 [%s] rt1 [%s] depth [%s]", a, b, c);
    if (rtv[0]) ID3D11RenderTargetView_GetResource(rtv[0], &r0);
    int is_bb = r0 && r0 == bb;
    if (r0) ID3D11Resource_Release(r0);
    for (int i = 0; i < 2; i++) if (rtv[i]) ID3D11RenderTargetView_Release(rtv[i]);
    if (dsv) ID3D11DepthStencilView_Release(dsv);
    if (bb) ID3D11Resource_Release(bb);
    return is_bb;
}

// half-size dump of the back buffer: bridge_bb_<capture>_<k>.raw (header: w, h, format; then 32-bit pixels)
static void dump_bb(const char *where) {
    if (g_dumps >= 40) return;
    ID3D11Resource *bb = backbuffer();
    if (!bb) { fprintf(g_cap, "    -> no back buffer to dump\n"); return; }
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)bb, &d);
    HRESULT hr;
    if (!g_staging) {
        D3D11_TEXTURE2D_DESC s = d;
        s.Usage = D3D11_USAGE_STAGING; s.BindFlags = 0; s.CPUAccessFlags = D3D11_CPU_ACCESS_READ; s.MiscFlags = 0;
        s.SampleDesc.Count = 1; s.SampleDesc.Quality = 0; s.MipLevels = 1; s.ArraySize = 1;
        if (FAILED(hr = ID3D11Device_CreateTexture2D(g_dev, &s, NULL, &g_staging))) {
            fprintf(g_cap, "    -> dump: staging texture failed 0x%08lx\n", hr);
            ID3D11Resource_Release(bb);
            return;
        }
    }
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)g_staging, bb);
    ID3D11Resource_Release(bb);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(hr = ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_staging, 0, D3D11_MAP_READ, 0, &m))) {
        fprintf(g_cap, "    -> dump: map failed 0x%08lx\n", hr);
        return;
    }
    wchar_t p[MAX_PATH];
    _snwprintf(p, MAX_PATH, L"%ls\\bridge_bb_%d_%02u.raw", g_dir, g_captures, g_dumps);
    FILE *f = _wfopen(p, L"wb");
    if (f) {
        unsigned hw = d.Width / 2, hh = d.Height / 2, hdr[3] = {hw, hh, d.Format};
        fwrite(hdr, 4, 3, f);
        static unsigned row[4096];
        for (unsigned y = 0; y < hh && hw <= 4096; y++) {
            const unsigned *src = (const unsigned *)((const unsigned char *)m.pData + (size_t)y * 2 * m.RowPitch);
            for (unsigned x = 0; x < hw; x++) row[x] = src[x * 2];
            fwrite(row, 4, hw, f);
        }
        fclose(f);
    }
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_staging, 0);
    fprintf(g_cap, "    -> back buffer dump %02u (%s)\n", g_dumps, where);
    g_dumps++;
}
// half-size grey picture of the scene depth (R24G8): bridge_depth_<capture>_<k>.raw, same layout as the back-buffer dumps
// (format 28); grey = (1 - depth) stretched between this picture's nearest and farthest pixel
static void dump_depth(const char *where) {
    static ID3D11Texture2D *st;
    static unsigned k;
    if (!g_scene_depth || k >= 8) return;
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)g_scene_depth, &d);
    if (d.Format != DXGI_FORMAT_R24G8_TYPELESS || d.SampleDesc.Count != 1) return;
    if (!st) {
        D3D11_TEXTURE2D_DESC s = d;
        s.Usage = D3D11_USAGE_STAGING; s.BindFlags = 0; s.CPUAccessFlags = D3D11_CPU_ACCESS_READ; s.MiscFlags = 0;
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &s, NULL, &st))) return;
    }
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)st, g_scene_depth);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) return;
    unsigned hw = d.Width / 2, hh = d.Height / 2, lo = 0xFFFFFF, hi = 0;
    for (unsigned y = 0; y < hh; y++)
        for (unsigned x = 0; x < hw; x++) {
            unsigned v = ((const unsigned *)((const unsigned char *)m.pData + (size_t)y * 2 * m.RowPitch))[x * 2] & 0xFFFFFF;
            if (v < 0xFFFFFF) { if (v < lo) lo = v; if (v > hi) hi = v; }
        }
    wchar_t p[MAX_PATH];
    _snwprintf(p, MAX_PATH, L"%ls\\bridge_depth_%d_%u.raw", g_dir, g_captures, k);
    FILE *f = _wfopen(p, L"wb");
    if (f) {
        unsigned hdr[3] = {hw, hh, 28};
        fwrite(hdr, 4, 3, f);
        static unsigned row[4096];
        for (unsigned y = 0; y < hh && hw <= 4096; y++) {
            const unsigned *src = (const unsigned *)((const unsigned char *)m.pData + (size_t)y * 2 * m.RowPitch);
            for (unsigned x = 0; x < hw; x++) {
                unsigned v = src[x * 2] & 0xFFFFFF;
                unsigned g = v >= 0xFFFFFF || hi <= lo ? 0 : 255 - (unsigned)(255.0 * (v - lo) / (hi - lo));
                row[x] = 0xFF000000u | g << 16 | g << 8 | g;
            }
            fwrite(row, 4, hw, f);
        }
        fclose(f);
    }
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)st, 0);
    fprintf(g_cap, "    -> depth dump %u (%s): nearest 0x%06X farthest 0x%06X\n", k, where, lo, hi);
    k++;
}

// ---- context hooks (vtable slots). d3d11.dll switches the immediate context between several vtables (offline test: Draw and
// Clear were not seen after the first frames) and can put its own entries back (Attila), so every table seen is patched
// and the original function is looked up by the table the call came through.
typedef void(STDMETHODCALLTYPE *omrt_fn)(ID3D11DeviceContext *, UINT, ID3D11RenderTargetView *const *, ID3D11DepthStencilView *);
typedef void(STDMETHODCALLTYPE *omrtuav_fn)(ID3D11DeviceContext *, UINT, ID3D11RenderTargetView *const *, ID3D11DepthStencilView *, UINT,
                                           UINT, ID3D11UnorderedAccessView *const *, const UINT *);
typedef void(STDMETHODCALLTYPE *cbs_fn)(ID3D11DeviceContext *, UINT, UINT, ID3D11Buffer *const *);
typedef void(STDMETHODCALLTYPE *crtv_fn)(ID3D11DeviceContext *, ID3D11RenderTargetView *, const FLOAT[4]);
typedef void(STDMETHODCALLTYPE *cdsv_fn)(ID3D11DeviceContext *, ID3D11DepthStencilView *, UINT, FLOAT, UINT8);
typedef HRESULT(STDMETHODCALLTYPE *map_fn)(ID3D11DeviceContext *, ID3D11Resource *, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE *);
typedef void(STDMETHODCALLTYPE *unmap_fn)(ID3D11DeviceContext *, ID3D11Resource *, UINT);
typedef void(STDMETHODCALLTYPE *upd_fn)(ID3D11DeviceContext *, ID3D11Resource *, UINT, const D3D11_BOX *, const void *, UINT, UINT);
typedef void(STDMETHODCALLTYPE *dip_fn)(ID3D11DeviceContext *, UINT, UINT, INT);           // DrawIndexed
typedef void(STDMETHODCALLTYPE *d_fn)(ID3D11DeviceContext *, UINT, UINT);                  // Draw
typedef void(STDMETHODCALLTYPE *dii_fn)(ID3D11DeviceContext *, UINT, UINT, UINT, INT, UINT); // DrawIndexedInstanced
typedef void(STDMETHODCALLTYPE *di_fn)(ID3D11DeviceContext *, UINT, UINT, UINT, UINT);      // DrawInstanced
typedef void(STDMETHODCALLTYPE *rsvp_fn)(ID3D11DeviceContext *, UINT, const D3D11_VIEWPORT *);
typedef void(STDMETHODCALLTYPE *rsgetvp_fn)(ID3D11DeviceContext *, UINT *, D3D11_VIEWPORT *);
typedef void(STDMETHODCALLTYPE *omdss_fn)(ID3D11DeviceContext *, ID3D11DepthStencilState *, UINT);
typedef void(STDMETHODCALLTYPE *disp_fn)(ID3D11DeviceContext *, UINT, UINT, UINT);
typedef void(STDMETHODCALLTYPE *psset_fn)(ID3D11DeviceContext *, ID3D11PixelShader *, ID3D11ClassInstance *const *, UINT);
enum { S_OMRT, S_OMRTUAV, S_VSCB, S_CRTV, S_CDSV, S_MAP, S_UNMAP, S_UPD, S_DIP, S_D, S_DII, S_DI, S_RSVP, S_RSGETVP, S_OMDSS, S_PSCB, S_VSSET, S_PSSAMP, S_DISP, S_PSSET, NSLOT };
static struct { void **vt; void *orig[NSLOT]; int rw; } g_vts[16];   // rw: the table is in writable memory
static unsigned g_rh_calls, g_rh_patches, g_rh_foreign;   // re-hook checks, slots hooked again, foreign slots left alone
static LONGLONG g_rh_ticks;                                // QPC ticks spent where a slot differed
static int g_nvt;
static void *orig_of(const void *obj, int s) {
    void **vt = *(void ***)obj;
    for (int i = 0; i < g_nvt; i++)
        if (g_vts[i].vt == vt && g_vts[i].orig[s]) return g_vts[i].orig[s];
    for (int i = 0; i < g_nvt; i++)   // a table that copied our entry from another one: that table's original
        if (g_vts[i].orig[s]) return g_vts[i].orig[s];
    return NULL;
}
#define ORIG(type, s, c) ((type)orig_of((c), (s)))
static void hook_context(void);
// a target set ends: dump the back buffer if the game drew into it
static void set_end(const char *why) {
    if (g_set_bb && g_set_draws) {
        char w[80];
        snprintf(w, sizeof w, "%u draws into the back buffer, %s", g_set_draws, why);
        dump_bb(w);
    }
}
static void set_begin(int is_bb) { g_set_bb = is_bb; g_set_draws = 0; }

static void log_targets(const char *what, const char *callers) {
    char t[600];
    set_end("before the next target change");
    int bb = targets(t, sizeof t);
    fprintf(g_cap, "#%u %s (game %s), draws so far %u: %s\n", g_sets++, what, callers, g_draws, t);
    set_begin(bb);
}

// live bridge, at every target change of the game's context (before it takes effect): the HUD-less point is the change
// after draws into the back buffer once the scene depth was drawn into this frame (captures: the 5 draws of the scene's
// copy into the back buffer; the banners and the HUD follow). Before the scene (4 back-buffer draws at frame start) it
// is not.
static void fl_hud(void);   // flash probe (below)
static void live_targets(ID3D11RenderTargetView *rtv0, ID3D11DepthStencilView *dsv) {
    if (g_lv_bb && g_lv_draws && g_scene_frame == g_frame && g_hud_frame != g_frame) {
        g_hud_frame = g_frame;
        fl_hud();
        jit_frame_end();   // the UI and what follows: the game's own viewports
        ID3D11Resource *bb = backbuffer();
        int cam = g_vp_frame == g_frame;
        if (!cam) g_br_novp++;   // no main camera seen: no motion vectors (a scaled frame still goes through DLSS, last camera)
        if (g_sr && !g_frame_scaled) g_br_native++;   // a scene frame not scaled (the first after loading): native, no DLSS
        else if ((cam || g_frame_scaled) && bb && g_scene_depth) {
            // below native size fgbridge.c puts the depth back to full size for what is drawn after (banners), on the
            // depth bound now; Rome 2's HUD-less point comes after a back-buffer pass without depth, so the scene's is bound
            if (g_capturing && g_cap) dump_depth("at the HUD-less point, before the bridge restores it");
            if ((g_rw != g_live_w || g_rh != g_live_h) && g_scene_dsv) ORIG(omrt_fn, S_OMRT, g_ctx)(g_ctx, 0, NULL, g_scene_dsv);
            fgb_hudless(bb, g_scene_depth, g_vp);
            g_br_hud++;
        }
        if (bb) ID3D11Resource_Release(bb);
    }
    ID3D11Resource *r = NULL, *bb = backbuffer();
    D3D11_RESOURCE_DIMENSION dim;
    D3D11_TEXTURE2D_DESC td = {0};
    if (rtv0) ID3D11RenderTargetView_GetResource(rtv0, &r);
    if (r) ID3D11Resource_GetType(r, &dim);
    if (r && dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) ID3D11Texture2D_GetDesc((ID3D11Texture2D *)r, &td);
    g_rt_w = td.Width;
    g_rt_h = td.Height;
    g_lv_bb = r && r == bb;
    g_rt_overlay = 0;
    for (unsigned k = 0; r && k < g_novres && k < 8; k++)
        if (g_ovres[k] == r && (g_ovkind[k] == 1 ? !dsv : g_ocean_frame != g_frame)) g_rt_overlay = g_ovkind[k];
    if (r) ID3D11Resource_Release(r);
    if (bb) ID3D11Resource_Release(bb);
    g_lv_draws = 0;
    g_lv_scene = 0;
    if (!dsv) return;
    ID3D11Resource *d = NULL;
    ID3D11DepthStencilView_GetResource(dsv, &d);
    ID3D11Resource_GetType(d, &dim);
    memset(&td, 0, sizeof td);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) ID3D11Texture2D_GetDesc((ID3D11Texture2D *)d, &td);
    if (!rtv0) { g_rt_w = td.Width; g_rt_h = td.Height; }
    if (td.Width == g_live_w && td.Height == g_live_h) {
        if (td.SampleDesc.Count > 1) {   // the game's MSAA: DLSS needs it off (its own anti-aliasing replaces it)
            if (!g_br_msaa++) g_log("frame bridge: the game renders with MSAA (%u samples) - set anti-aliasing off in the game's "
                                    "graphics options for DLSS; until then frames go out without motion vectors", td.SampleDesc.Count);
        } else {
            g_lv_scene = 1;
            if (g_scene_frame != g_frame) g_br_scene_frames++;
            g_scene_frame = g_frame;
            if (d != g_scene_depth) {
                if (g_scene_depth) ID3D11Resource_Release(g_scene_depth);
                if (g_scene_dsv) ID3D11DepthStencilView_Release(g_scene_dsv);
                g_scene_dsv = NULL;   // made again for the new depth below
                g_scene_depth = d;
                d = NULL;   // the reference is kept
            }
            if (!g_scene_dsv) {
                // our own writable view of it, for the depth pass below native size: the game binds the depth read-only
                // in its lighting / fog passes (to sample it at the same time), and writes through that view are dropped -
                // the restored depth stayed shrunk and cut the banners (capture 2026-10-29)
                D3D11_DEPTH_STENCIL_VIEW_DESC vd;
                ID3D11DepthStencilView_GetDesc(dsv, &vd);
                vd.Flags = 0;
                if (FAILED(ID3D11Device_CreateDepthStencilView(g_dev, g_scene_depth, &vd, &g_scene_dsv))) g_scene_dsv = NULL;
            }
        }
    }
    if (d) ID3D11Resource_Release(d);
}

static void STDMETHODCALLTYPE w_omrt(ID3D11DeviceContext *c, UINT n, ID3D11RenderTargetView *const *rtv, ID3D11DepthStencilView *dsv) { if (r2m_internal) { ORIG(omrt_fn, S_OMRT, c)(c, n, rtv, dsv); return; }
    if (g_live_state == 1 && c == g_ctx) live_targets(n && rtv ? rtv[0] : NULL, dsv);
    ORIG(omrt_fn, S_OMRT, c)(c, n, rtv, dsv);
    // d3d11 puts its own Draw entries back into the table during a frame (bridgelive_test with the real bridge: after
    // frame 0 the composite's draws were no longer seen, the vtable pointer unchanged): hooked again at each target change
    if ((g_capturing || g_camera_left || g_live_state == 1) && c == g_ctx) hook_context();
    if (g_live_state == 1 && c == g_ctx) jit_apply(c, 0);
    if (g_capturing && c == g_ctx) { CALLERS(cs); log_targets("OMSetRenderTargets", cs); }
}
static void STDMETHODCALLTYPE w_omrtuav(ID3D11DeviceContext *c, UINT n, ID3D11RenderTargetView *const *rtv, ID3D11DepthStencilView *dsv,
                                        UINT us, UINT un, ID3D11UnorderedAccessView *const *uav, const UINT *ic) { if (r2m_internal) { ORIG(omrtuav_fn, S_OMRTUAV, c)(c, n, rtv, dsv, us, un, uav, ic); return; }
    if (g_live_state == 1 && c == g_ctx && n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) live_targets(n && rtv ? rtv[0] : NULL, dsv);
    ORIG(omrtuav_fn, S_OMRTUAV, c)(c, n, rtv, dsv, us, un, uav, ic);
    if (g_live_state == 1 && c == g_ctx) { hook_context(); jit_apply(c, 0); }
    if (g_capturing && c == g_ctx && n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) { CALLERS(cs); log_targets("OMSetRenderTargetsAndUAVs", cs); }
}
static void STDMETHODCALLTYPE w_vscb(ID3D11DeviceContext *c, UINT start, UINT n, ID3D11Buffer *const *b) { if (r2m_internal) { ORIG(cbs_fn, S_VSCB, c)(c, start, n, b); return; }
    ORIG(cbs_fn, S_VSCB, c)(c, start, n, b);
    // live too: in game the camera's Map / Unmap were missed in about 2 of 3 frames with the re-hook at target changes
    // only (first in-game run, 2026-10-28); the capture, which also re-hooked here, saw every camera write
    if ((g_capturing || g_camera_left || g_live_state == 1) && c == g_ctx) hook_context();
    if (g_capturing && c == g_ctx && start == 0 && n && b && b[0]) {
        D3D11_BUFFER_DESC d;
        ID3D11Buffer_GetDesc(b[0], &d);
        CALLERS(cs);
        fprintf(g_cap, "    VS cb0 = @%p (%u bytes) (game %s), draws so far %u\n", (void *)b[0], d.ByteWidth, cs, g_draws);
    }
}
static void STDMETHODCALLTYPE w_crtv(ID3D11DeviceContext *c, ID3D11RenderTargetView *v, const FLOAT col[4]) { if (r2m_internal) { ORIG(crtv_fn, S_CRTV, c)(c, v, col); return; }
    ORIG(crtv_fn, S_CRTV, c)(c, v, col);
    if (g_capturing && c == g_ctx) {
        char t[160];
        ID3D11Resource *bb = backbuffer();
        desc_of((ID3D11View *)v, t, sizeof t, bb);
        if (bb) ID3D11Resource_Release(bb);
        CALLERS(cs);
        fprintf(g_cap, "    clear colour [%s] (game %s)\n", t, cs);
    }
}
static void STDMETHODCALLTYPE w_cdsv(ID3D11DeviceContext *c, ID3D11DepthStencilView *v, UINT fl, FLOAT depth, UINT8 st) { if (r2m_internal) { ORIG(cdsv_fn, S_CDSV, c)(c, v, fl, depth, st); return; }
    ORIG(cdsv_fn, S_CDSV, c)(c, v, fl, depth, st);
    if (g_capturing && c == g_ctx) {
        char t[160];
        desc_of((ID3D11View *)v, t, sizeof t, NULL);
        CALLERS(cs);
        fprintf(g_cap, "    clear depth [%s] flags %u to %g stencil %u (game %s)\n", t, fl, depth, st, cs);
    }
}

// camera constant buffers: constant buffers of 544..576 bytes (camera_VS_PS uses 548)
static ID3D11Resource *g_cam_res;
static void *g_cam_ptr;
static int is_cam_buffer(ID3D11Resource *r) {
    D3D11_RESOURCE_DIMENSION dim;
    ID3D11Resource_GetType(r, &dim);
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return 0;
    D3D11_BUFFER_DESC d;
    ID3D11Buffer_GetDesc((ID3D11Buffer *)r, &d);
    return (d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) && d.ByteWidth >= 544 && d.ByteWidth <= 576;
}
static void cam_record(ID3D11Resource *r, const void *data, const char *how) {
    unsigned cs[3] = {0};
    game_callers((const unsigned *)__builtin_frame_address(0), cs, 3);
    unsigned rec[8] = {0x43414D31u /* "CAM1" */, g_frame, (unsigned)(UINT_PTR)r, cs[0], cs[1], cs[2], 544, 0};
    fwrite(rec, 4, 8, g_cam);
    fwrite(data, 1, 544, g_cam);
    g_cam_writes++;
    if (g_capturing) {
        const float *f = (const float *)data;
        fprintf(g_cap, "    camera write %s @%p (game +0x%X +0x%X): position (%.2f %.2f %.2f) screen (%.0f %.0f) viewport (%.0f %.0f %.0f %.0f) vp row0 (%.4f %.4f %.4f %.4f), draws so far %u\n",
                how, (void *)r, cs[0], cs[1], f[0], f[1], f[2], f[108], f[109], f[116], f[117], f[118], f[119], f[36], f[37], f[38], f[39], g_draws);
    }
}
// a camera write while the scene depth is bound is the main view (captures: the shadow, reflection and environment views
// are written with other depth buffers or none): its view_projection (@144) is the frame's camera
static int g_mp;                          // the motion probe is on (below; 2: capture at once, for the offline test)
static FILE *g_mpf;                       // its capture frame's file
static void mp_map(ID3D11Resource *r, D3D11_MAP t);
static float g_cam_block[140];            // the main camera block, kept for the motion probe
// flash probe (debug view on, DLSS_LOG 27): every main-camera candidate of the last 8 frames, to explain a frame whose
// motion vectors jump over the whole screen
typedef struct { const void *res; unsigned draws; float nearz, farz, vp[16]; } FlCand;
typedef struct { unsigned frame, n, hud, gap; float change, vp[16]; FlCand c[24]; } FlFrame;
static FlFrame g_fl[8];
static ID3D11Resource *g_fl_res;          // the buffer of the camera write being taken
static unsigned g_fl_last_hud = ~0u;
static float g_fl_last_vp[16];
static FlFrame *fl_frame(void) {
    FlFrame *f = &g_fl[g_frame & 7];
    if (f->frame != g_frame) { memset(f, 0, sizeof *f); f->frame = g_frame; }
    return f;
}
static void live_camera(const void *cam) {
    memcpy(g_vp, (const char *)cam + 144, 64);
    g_vp_frame = g_frame;
    if (g_mp) memcpy(g_cam_block, cam, sizeof g_cam_block);
    if (g_view) {
        FlFrame *f = fl_frame();
        if (f->n < 24) {
            FlCand *k = &f->c[f->n];
            k->res = g_fl_res; k->draws = g_draws;
            k->nearz = ((const float *)cam)[100]; k->farz = ((const float *)cam)[101];
            memcpy(k->vp, (const char *)cam + 144, 64);
        }
        f->n++;
    }
}
static float fl_change(const float *a, const float *b) {   // relative size of the difference of two matrices
    double d = 0, s = 0;
    for (int i = 0; i < 16; i++) d += (double)(a[i] - b[i]) * (a[i] - b[i]), s += (double)a[i] * a[i];
    return s > 0 ? (float)sqrt(d / s) : 0;
}
static void fl_hud(void) {   // at the HUD-less point: the camera taken, its change from the last HUD-less frame's
    if (!g_view) return;
    FlFrame *f = fl_frame();
    f->hud = 1;
    memcpy(f->vp, g_vp, 64);
    f->gap = g_fl_last_hud == ~0u ? 0 : g_frame - g_fl_last_hud;
    f->change = g_fl_last_hud == ~0u ? 0 : fl_change(g_vp, g_fl_last_vp);
    memcpy(g_fl_last_vp, g_vp, 64);
    g_fl_last_hud = g_frame;
}
static void fl_log_frame(const FlFrame *f, const char *what) {
    g_log("    %s frame %u: HUD-less %d, frames since the last HUD-less %u, camera change %.4f, %u camera writes with the scene depth:",
          what, f->frame, f->hud, f->gap, f->change, f->n);
    for (unsigned i = 0; i < f->n && i < 24; i++) {
        const FlCand *k = &f->c[i];
        g_log("      #%u buffer %p at draw %u: near %g far %g, differs from the taken %.4f; rows %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | "
              "%.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f", i, k->res, k->draws, k->nearz, k->farz, fl_change(k->vp, f->vp), k->vp[0], k->vp[1],
              k->vp[2], k->vp[3], k->vp[4], k->vp[5], k->vp[6], k->vp[7], k->vp[8], k->vp[9], k->vp[10], k->vp[11], k->vp[12], k->vp[13],
              k->vp[14], k->vp[15]);
    }
}
static float fl_h2f(unsigned short h) {
    unsigned e = (h >> 10) & 31, m = h & 1023;
    float v = e == 31 ? 65504 : e ? ldexpf((float)(m | 1024), (int)e - 25) : ldexpf((float)m, -24);   // inf / NaN read as the largest
    return h >> 15 ? -v : v;
}
static unsigned g_fl_flashes, g_fl_multi, g_fl_frames;
static float g_fl_max_change;
// at Present (debug view on, a frame with vectors): 9 motion vectors copied for reading 3 frames later (no wait); a frame
// whose median motion is over 8 px and 4x the recent median is a flash: logged with its camera candidates
static void fl_present(ID3D11Texture2D *mv) {
    static ID3D11Texture2D *st[4];
    static unsigned st_frame[4], rw[4], rh[4];
    static float hist[30];
    static unsigned nhist;
    if (!st[0]) {
        D3D11_TEXTURE2D_DESC d = {9, 1, 1, 1, DXGI_FORMAT_R16G16_FLOAT, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
        for (int i = 0; i < 4; i++) if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &st[i]))) return;
        for (int i = 0; i < 4; i++) st_frame[i] = ~0u;
    }
    FlFrame *f = fl_frame();
    g_fl_frames++;
    int other = 0;   // a frame with a camera candidate other than the one taken
    for (unsigned i = 0; i < f->n && i < 24; i++) other |= fl_change(f->c[i].vp, f->vp) > 1e-5f;
    g_fl_multi += other;
    if (f->change > g_fl_max_change) g_fl_max_change = f->change;
    // the copy 3 frames ago, if the GPU has finished it
    int s = (int)(g_frame & 3);
    D3D11_MAPPED_SUBRESOURCE m;
    if (st_frame[s] != ~0u && SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)st[s], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) {
        float g[9], sorted[9];
        for (int i = 0; i < 9; i++) {
            const unsigned short *h = (const unsigned short *)m.pData + i * 2;
            float x = fl_h2f(h[0]), y = fl_h2f(h[1]);
            g[i] = sorted[i] = sqrtf(x * x + y * y);
        }
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)st[s], 0);
        for (int i = 1; i < 9; i++) for (int j = i; j > 0 && sorted[j] < sorted[j - 1]; j--) { float t = sorted[j]; sorted[j] = sorted[j - 1]; sorted[j - 1] = t; }
        float med = sorted[4], hm = 0;
        if (nhist) {
            float hs[30];
            unsigned n = nhist < 30 ? nhist : 30;
            memcpy(hs, hist, n * sizeof *hs);
            for (unsigned i = 1; i < n; i++) for (unsigned j = i; j > 0 && hs[j] < hs[j - 1]; j--) { float t = hs[j]; hs[j] = hs[j - 1]; hs[j - 1] = t; }
            hm = hs[n / 2];
        }
        unsigned fr = st_frame[s];
        if (nhist >= 10 && med > 8 && med > 4 * hm && g_fl_flashes++ < 30) {
            g_log("motion flash at frame %u (render %ux%u): median motion %.1f px (recent median %.1f); the 9 points %.1f %.1f %.1f | %.1f %.1f "
                  "%.1f | %.1f %.1f %.1f", fr, rw[s], rh[s], med, hm, g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7], g[8]);
            const FlFrame *a = &g_fl[fr & 7];
            if (a->frame == fr) {
                fl_log_frame(a, "this");
                for (unsigned b = fr - 1, k = 0; k < 6; b--, k++)   // and the last HUD-less frame before it
                    if (g_fl[b & 7].frame == b && g_fl[b & 7].hud) { fl_log_frame(&g_fl[b & 7], "previous HUD-less"); break; }
            } else g_log("    (its camera record is gone)");
        }
        hist[nhist++ % 30] = med;
        st_frame[s] = ~0u;
    }
    if (st_frame[s] == ~0u) {   // this frame's 9 points (a 3 x 3 grid over the render size)
        for (int i = 0; i < 9; i++) {
            UINT x = g_rw * (1 + 2 * (i % 3)) / 6, y = g_rh * (1 + 2 * (i / 3)) / 6;
            D3D11_BOX b = {x, y, 0, x + 1, y + 1, 1};
            ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)st[s], 0, (UINT)i, 0, 0, (ID3D11Resource *)mv, 0, &b);
        }
        st_frame[s] = g_frame; rw[s] = g_rw; rh[s] = g_rh;
    }
}
static HRESULT STDMETHODCALLTYPE w_map(ID3D11DeviceContext *c, ID3D11Resource *r, UINT sub, D3D11_MAP t, UINT fl, D3D11_MAPPED_SUBRESOURCE *m) { if (r2m_internal) return ORIG(map_fn, S_MAP, c)(c, r, sub, t, fl, m);
    LARGE_INTEGER t0 = {0}, t1;
    if (g_live_state == 1) QueryPerformanceCounter(&t0);
    HRESULT hr = ORIG(map_fn, S_MAP, c)(c, r, sub, t, fl, m);
    if (t0.QuadPart) {
        QueryPerformanceCounter(&t1);
        LONGLONG d = t1.QuadPart - t0.QuadPart;
        g_t_map += d;
        if (d > g_t_map_max) g_t_map_max = d;
        g_n_map++;
    }
    if (((g_camera_left && g_cam) || g_live_state == 1) && c == g_ctx && SUCCEEDED(hr) && m && (t == D3D11_MAP_WRITE_DISCARD || t == D3D11_MAP_WRITE) &&
        is_cam_buffer(r)) {
        g_cam_res = r;
        g_cam_ptr = m->pData;
    }
    if (g_mpf && c == g_ctx && SUCCEEDED(hr)) mp_map(r, t);   // motion probe capture frame: the vertex buffer Map pattern
    return hr;
}
static void STDMETHODCALLTYPE w_unmap(ID3D11DeviceContext *c, ID3D11Resource *r, UINT sub) { if (r2m_internal) { ORIG(unmap_fn, S_UNMAP, c)(c, r, sub); return; }
    if (g_cam_res == r && g_cam_ptr && c == g_ctx) {
        if (g_cam) cam_record(r, g_cam_ptr, "map");
        if (g_live_state == 1) g_br_camw++, g_br_camw_scene += g_lv_scene, cam_copy_write(r, g_cam_ptr);
        g_fl_res = r;
        if (g_lv_scene && g_hud_frame != g_frame) live_camera(g_cam_ptr);   // not after the HUD-less point (a UI portrait, G1)
        g_cam_res = NULL;
        g_cam_ptr = NULL;
    }
    ORIG(unmap_fn, S_UNMAP, c)(c, r, sub);
}
static void STDMETHODCALLTYPE w_upd(ID3D11DeviceContext *c, ID3D11Resource *r, UINT sub, const D3D11_BOX *box, const void *data, UINT rp, UINT dp) { if (r2m_internal) { ORIG(upd_fn, S_UPD, c)(c, r, sub, box, data, rp, dp); return; }
    if (((g_camera_left && g_cam) || g_live_state == 1) && c == g_ctx && !box && data && is_cam_buffer(r)) {
        if (g_camera_left && g_cam) cam_record(r, data, "update");
        if (g_live_state == 1) g_br_camw++, g_br_camw_scene += g_lv_scene, cam_copy_write(r, data);
        g_fl_res = r;
        if (g_lv_scene && g_hud_frame != g_frame) live_camera(data);
    }
    LARGE_INTEGER t0 = {0}, t1;
    if (g_live_state == 1) QueryPerformanceCounter(&t0);
    ORIG(upd_fn, S_UPD, c)(c, r, sub, box, data, rp, dp);
    if (t0.QuadPart) {
        QueryPerformanceCounter(&t1);
        LONGLONG d = t1.QuadPart - t0.QuadPart;
        g_t_upd += d;
        if (d > g_t_upd_max) g_t_upd_max = d;
        g_n_upd++;
    }
}
// ---- Motion probe (per-object motion vectors, game run G1 of dlss/DLSS_LOG.md 19; fgbridge_on.txt line 4 "motionprobe").
// Records by itself, once per scene kind (battle: camera far plane > 2000, campaign map: below), after animated models
// have been drawn for 20 s, three consecutive frames into motion_<kind>_<k>.bin: every main-view draw (scene depth
// bound) whose vertex shader animates (its bytecode names the cbuffer variant_animation_size) with its input layout,
// vertex / index / instance buffers, draw arguments, VS constant buffers and textures, GPU copies of its instance records,
// of the animation textures and of the meshes (each buffer once); the main camera block; the Maps on vertex buffers; and
// a summary of all main-view draws by vertex shader. Animated vertex shaders' bytecode goes to motion_vs_<checksum>.bin.
// Chunks: u32 tag, u32 size, payload (tools/motion_dump.py reads them). Capture frames take seconds (staging copies).
typedef HRESULT(STDMETHODCALLTYPE *cvs_fn)(ID3D11Device *, const void *, SIZE_T, ID3D11ClassLinkage *, ID3D11VertexShader **);
typedef HRESULT(STDMETHODCALLTYPE *cil_fn)(ID3D11Device *, const D3D11_INPUT_ELEMENT_DESC *, UINT, const void *, SIZE_T, ID3D11InputLayout **);
typedef void(STDMETHODCALLTYPE *vsset_fn)(ID3D11DeviceContext *, ID3D11VertexShader *, ID3D11ClassInstance *const *, UINT);
static cvs_fn r_cvs;
static cil_fn r_cil;
typedef struct { const void *vs; unsigned long long sum; int anim, type, ocean; unsigned draws, insts; } MpVs;   // type: motion pass shader (-1 none)
static MpVs g_mpvs[4096];
static volatile LONG g_nmpvs;
static volatile LONG g_mph[8192];          // shader pointer -> g_mpvs index + 1 (open addressing)
typedef struct { const void *il; UINT n; D3D11_INPUT_ELEMENT_DESC e[16]; char name[16][24]; } MpIl;
static MpIl g_mpil[1024];
static volatile LONG g_nmpil;
static const void *g_cur_vs;              // the vertex shader bound (VSSetShader)
static MpVs *g_cur_mpvs;                  // its entry
static unsigned g_mp_frame_anim;          // animated main-view draws this frame
static int g_mp_kind = -1, g_mp_done[2], g_mp_left, g_mp_k;
static DWORD g_mp_seen[2];
static FILE *g_mpmesh;                    // the meshes (each buffer once per capture)
static const void *g_mp_wf[2048], *g_mp_ws[4096];   // written already: this frame (textures, layouts), this capture (meshes)
static unsigned g_mp_nwf, g_mp_nws;
typedef struct { const void *buf; UINT type, size, n; } MpMap;
static MpMap g_mpmaps[256];
static unsigned g_nmpmaps;
static unsigned long long mp_sum(const void *bc, SIZE_T len) {   // the DXBC checksum (bytes 4..11), else FNV-1a
    if (len >= 20 && !memcmp(bc, "DXBC", 4)) { unsigned long long s; memcpy(&s, (const char *)bc + 4, 8); return s; }
    unsigned long long h = 1469598103934665603ull;
    for (SIZE_T i = 0; i < len; i++) h = (h ^ ((const unsigned char *)bc)[i]) * 1099511628211ull;
    return h;
}
static int bc_has(const void *bc, SIZE_T len, const char *s) {   // the bytecode names s (its RDEF: cbuffers, variables)
    SIZE_T n = strlen(s);
    for (SIZE_T k = 0; k + n <= len; k++) if (!memcmp((const char *)bc + k, s, n)) return 1;
    return 0;
}
// Naval battles (Attila AGENT_LOG 128, same engine): before the main view the game draws the wake foam and the selection
// capsules into two screen-sized targets without depth, which only the ocean's pixel shaders read (colour_overlay_texture,
// ui_colour_overlay_texture: data_rome2.pack, no other dx11 shader names them), at uv = clip.xy / clip.w * (0.5, -0.5) +
// 0.5 from the ocean's vertex shader: full-screen coordinates. Shrunk with the scene they landed in the top-left corner
// and showed away from the ships. So the textures the ocean samples there are learned at its draws (their slots differ
// by water quality: t6 / t7 low, t7 / t9 ultra; read from each pixel shader's RDEF) and drawn into at full size.
// The water reflection too (only the ocean names reflection_texture; user: "the water ... cuts off so close to the
// cam"): a mirrored view drawn into a half-size target, read at uv = (SV_Position * 1 / screen + ripple, then y -> 1 - y)
// (dx11 ultra: add r6.z, -r6.y, l(1.0)). Shrunk into the top-left, the flipped lookup missed it for the upper part of
// the water. Shrunk into the bottom-left instead, the game's own lookup lands exactly on it (row H - r * y_full); its
// pixel shaders get a camera copy patched for that corner. Only until the frame's first ocean draw (it reads it there;
// the game may use the same texture for something else later in the frame).
// Lakes, sea on land maps and rivers (waterplane.hlsl, rigidriver.hlsl; user: "water is messed up" at Quality, a coastal
// land battle) read theirs the same way under another name, s_terrain_reflection (t2: sample_l at (x, 1 - y)): the
// mirrored terrain / sky pass and the screen-space pass into it, then the water. Learned at the water's own draws (its
// vertex shader is not the ocean's).
typedef HRESULT(STDMETHODCALLTYPE *cps_fn)(ID3D11Device *, const void *, SIZE_T, ID3D11ClassLinkage *, ID3D11PixelShader **);
static cps_fn r_cps;
static struct { const void *ps; unsigned slots, rslots, tmcb; int ssr; } g_ovps[64];   // the ocean's pixel shaders (overlay / reflection
                                                                                // slots) and the screen-space reflection one (below)
static volatile LONG g_novps;
#define overlay_slots(bc, len, name) rdef_slots(bc, len, name, 2)
static unsigned rdef_slots(const void *bc, SIZE_T len, const char *name, unsigned type) {   // slots of the resources of a type
                                                                                         // (2 texture, 0 cbuffer) named *name* (DXBC RDEF)
    const unsigned char *b = bc;
    unsigned nc, off, slots = 0;
    if (len < 32 || memcmp(b, "DXBC", 4)) return 0;
    memcpy(&nc, b + 28, 4);
    for (unsigned i = 0; i < nc && 32 + 4 * i + 4 <= len; i++) {
        memcpy(&off, b + 32 + 4 * i, 4);
        if (off > len - 24 || memcmp(b + off, "RDEF", 4)) continue;
        const unsigned char *r = b + off + 8;
        unsigned rlen, nb, bo;
        memcpy(&rlen, b + off + 4, 4);
        memcpy(&nb, r + 8, 4);
        memcpy(&bo, r + 12, 4);
        if (rlen > len - off - 8) return 0;
        for (unsigned j = 0; j < nb && bo + 32 * j + 32 <= rlen; j++) {
            unsigned e[8];
            memcpy(e, r + bo + 32 * j, 32);   // name, type (2: texture), return type, dimension, samples, bind point, count, flags
            if (e[1] != type || e[5] >= 32 || e[0] >= rlen) continue;
            const char *nm = (const char *)r + e[0];
            if (memchr(nm, 0, rlen - e[0]) && strstr(nm, name)) slots |= 1u << e[5];
        }
    }
    return slots;
}
static struct { const void *ps; unsigned long long sum; } g_capps[8192];   // every pixel shader's DXBC checksum (captures)
static volatile LONG g_ncapps;
// Pixel shaders whose camera block is not in slot 0 (RDEF; 26 kinds in the packs: the 3D UI's forward lighting, campaign
// borders, MLAA, the low-quality sprites, the terrain's virtual-texture page touch): ps_cb_apply only swaps slot 0, so
// below native size these get the patched copy at their draws (cs_begin). slot 0xFF: a shader created at a freed one's
// address that has it in slot 0. pt: ps30_page_touch (cs_begin, below).
static struct { const void *volatile ps; unsigned char slot, pt; } g_camps[1024];
// Per pixel shader, what the per-draw fixes look for (set at creation), and the bound one's (PSSetShader, w_psset): draws
// by any other shader skip them (asking d3d11 for the shader at every draw cost 70-80 ns a draw: 0.46 ms a frame in a
// 5700-draw naval battle). The fixes still check the shader they are given.
enum { PSF_WATER = 1, PSF_TM = 2, PSF_CAM = 4 };
static struct { const void *volatile ps; unsigned char f; } g_psf[2048];
static unsigned g_cur_psf;
static void psf_set(const void *ps, unsigned f) {   // loader threads too
    for (unsigned h = ((unsigned)(UINT_PTR)ps >> 4) & 2047, k = 0; k < 2048; h = (h + 1) & 2047, k++) {
        if (g_psf[h].ps == ps || (!g_psf[h].ps && !InterlockedCompareExchangePointer((void *volatile *)&g_psf[h].ps, (void *)ps, NULL))) {
            g_psf[h].f = (unsigned char)f;
            return;
        }
    }   // shortcut: 2048 at most (the packs have about 200 such shaders), the rest skip the fixes
}
static unsigned psf_get(const void *ps) {
    for (unsigned h = ((unsigned)(UINT_PTR)ps >> 4) & 2047, k = 0; k < 2048 && g_psf[h].ps; h = (h + 1) & 2047, k++)
        if (g_psf[h].ps == ps) return g_psf[h].f;
    return 0;
}
static void STDMETHODCALLTYPE w_psset(ID3D11DeviceContext *c, ID3D11PixelShader *ps, ID3D11ClassInstance *const *ci, UINT n) {
    if (!r2m_internal && c == g_ctx) g_cur_psf = ps ? psf_get(ps) : 0;
    ORIG(psset_fn, S_PSSET, c)(c, ps, ci, n);
}
static void camps_set(const void *ps, unsigned slot, int pt) {   // loader threads too
    for (unsigned h = ((unsigned)(UINT_PTR)ps >> 4) & 1023, k = 0; k < 1024; h = (h + 1) & 1023, k++) {
        if (g_camps[h].ps == ps || (!g_camps[h].ps && !InterlockedCompareExchangePointer((void *volatile *)&g_camps[h].ps, (void *)ps, NULL))) {
            g_camps[h].slot = (unsigned char)slot; g_camps[h].pt = (unsigned char)pt;
            return;
        }
    }   // shortcut: 1024 at most (the packs have 113), the rest keep the game's camera
}
static int camps_find(const void *ps, unsigned *slot, int *pt) {
    for (unsigned h = ((unsigned)(UINT_PTR)ps >> 4) & 1023, k = 0; k < 1024; h = (h + 1) & 1023, k++) {
        if (!g_camps[h].ps) return 0;
        if (g_camps[h].ps == ps) { *slot = g_camps[h].slot; *pt = g_camps[h].pt; return g_camps[h].slot != 0xFF; }
    }
    return 0;
}
static int camps_has(const void *ps) {   // an entry for this address, valid or not
    for (unsigned h = ((unsigned)(UINT_PTR)ps >> 4) & 1023, k = 0; k < 1024 && g_camps[h].ps; h = (h + 1) & 1023, k++)
        if (g_camps[h].ps == ps) return 1;
    return 0;
}
static HRESULT STDMETHODCALLTYPE w_cps(ID3D11Device *d, const void *bc, SIZE_T len, ID3D11ClassLinkage *cl, ID3D11PixelShader **out) {
    HRESULT hr = r_cps(d, bc, len, cl, out);
    if (SUCCEEDED(hr) && out && *out && bc && len >= 12 && !memcmp(bc, "DXBC", 4)) {
        LONG i = InterlockedIncrement(&g_ncapps) - 1;
        if (i < 8192) { memcpy(&g_capps[i].sum, (const char *)bc + 4, 8); g_capps[i].ps = *out; }   // shortcut: never freed
    }
    int ok = SUCCEEDED(hr) && out && *out && bc;
    unsigned slots = ok ? overlay_slots(bc, len, "colour_overlay_texture") : 0, rslots = ok ? overlay_slots(bc, len, "reflection_texture") | overlay_slots(bc, len, "s_terrain_reflection") : 0;
    int ssr = ok && bc_has(bc, len, "screen_space_refl_params");
    unsigned tmcb = ok && bc_has(bc, len, "g_render_target_recip_size") ? rdef_slots(bc, len, "render_target_dims_buffer", 0) : 0;   // tone mapper
    unsigned cams = ok ? rdef_slots(bc, len, "camera_VS_PS", 0) : 0;
    if (cams && !(cams & 1)) {
        unsigned sl = 1;
        while (!(cams >> sl & 1)) sl++;
        camps_set(*out, sl, bc_has(bc, len, "g_page_touch_constants"));
    } else if (ok && camps_has(*out)) camps_set(*out, 0xFF, 0);
    unsigned psf = (slots | rslots ? PSF_WATER : 0) | (tmcb ? PSF_TM : 0) | (cams && !(cams & 1) ? PSF_CAM : 0);
    if (ok && (psf || psf_get(*out))) psf_set(*out, psf);   // (a shader at a freed one's address: its entry cleared)
    if (slots || rslots || ssr || tmcb) {
        LONG i = InterlockedIncrement(&g_novps) - 1;   // loader threads too
        if (i < 64) { g_ovps[i].slots = slots; g_ovps[i].rslots = rslots; g_ovps[i].tmcb = tmcb; g_ovps[i].ssr = ssr; g_ovps[i].ps = *out; }   // shortcut: 64 at most (Rome 2: 4 per shadow mode), never freed
    }
    return hr;
}
// capture: at the frame's first ocean draw, every texture its pixel shader reads, and the screen-proportioned ones dumped
// (bridge_ocean_<capture>_t<slot>.raw: u32 width, height, DXGI format, then the rows as stored)
static void cap_ocean(ID3D11DeviceContext *c, unsigned slots, unsigned rslots) {
    ID3D11ShaderResourceView *s[16] = {0};
    ID3D11DeviceContext_PSGetShaderResources(c, 0, 16, s);
    fprintf(g_cap, "    ocean draw %u (overlay slots 0x%x, reflection slots 0x%x) reads:\n", g_draws, slots, rslots);
    for (int i = 0; i < 16; i++) {
        if (!s[i]) continue;
        char t[160];
        ID3D11Resource *r = NULL;
        desc_of((ID3D11View *)s[i], t, sizeof t, NULL);
        ID3D11ShaderResourceView_GetResource(s[i], &r);
        D3D11_RESOURCE_DIMENSION dim;
        ID3D11Resource_GetType(r, &dim);
        D3D11_TEXTURE2D_DESC d = {0};
        if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) ID3D11Texture2D_GetDesc((ID3D11Texture2D *)r, &d);
        // only uncompressed 4- and 8-byte formats (a block-compressed texture has a quarter of the rows: the 21:28 crash)
        unsigned bpp = d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8
                     : d.Format == DXGI_FORMAT_R11G11B10_FLOAT || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                       d.Format == DXGI_FORMAT_R10G10B10A2_UNORM || d.Format == DXGI_FORMAT_R32_FLOAT ? 4 : 0;
        int dump = bpp && d.Width >= g_live_w / 4 && d.Width <= 4096 && d.SampleDesc.Count == 1 && d.ArraySize == 1;
        fprintf(g_cap, "      t%d [%s]%s\n", i, t, dump ? " dumped" : "");
        ID3D11Texture2D *st = NULL;
        D3D11_MAPPED_SUBRESOURCE m;
        D3D11_TEXTURE2D_DESC sd = d;
        sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0; sd.MipLevels = 1;
        if (dump && SUCCEEDED(ID3D11Device_CreateTexture2D(g_dev, &sd, NULL, &st))) {
            ID3D11DeviceContext_CopySubresourceRegion(c, (ID3D11Resource *)st, 0, 0, 0, 0, r, 0, NULL);
            if (SUCCEEDED(ID3D11DeviceContext_Map(c, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
                wchar_t p[MAX_PATH];
                _snwprintf(p, MAX_PATH, L"%ls\\bridge_ocean_%d_t%d.raw", g_dir, g_captures, i);
                FILE *f = _wfopen(p, L"wb");
                if (f) {
                    unsigned hdr[3] = {d.Width, d.Height, d.Format};
                    fwrite(hdr, 4, 3, f);
                    for (unsigned y = 0; y < d.Height; y++) fwrite((const unsigned char *)m.pData + (size_t)y * m.RowPitch, bpp, d.Width, f);
                    fclose(f);
                }
                ID3D11DeviceContext_Unmap(c, (ID3D11Resource *)st, 0);
            }
            ID3D11Texture2D_Release(st);
        }
        if (r) ID3D11Resource_Release(r);
        ID3D11ShaderResourceView_Release(s[i]);
    }
}
static void overlay_learn(ID3D11DeviceContext *c) {   // at the scene's draws: the ocean's, the land water's
    ID3D11PixelShader *ps = NULL;
    ID3D11DeviceContext_PSGetShader(c, &ps, NULL, NULL);
    unsigned slots = 0, rslots = 0;
    for (LONG i = g_novps < 64 ? g_novps : 64; ps && i-- > 0;) if (g_ovps[i].ps == ps) slots = g_ovps[i].slots, rslots = g_ovps[i].rslots;
    if (ps) ID3D11PixelShader_Release(ps);
    if (!(slots | rslots) && !(g_cur_mpvs && g_cur_mpvs->ocean)) return;   // not water
    if (g_capturing && g_cap && g_ocean_frame != g_frame) cap_ocean(c, slots, rslots);
    g_ocean_frame = g_frame;
    for (UINT s = 0; s < 32 && (slots | rslots) >> s; s++) {
        if (!((slots | rslots) >> s & 1)) continue;
        int kind = slots >> s & 1 ? 1 : 2;
        ID3D11ShaderResourceView *v = NULL;
        ID3D11Resource *r = NULL;
        ID3D11DeviceContext_PSGetShaderResources(c, s, 1, &v);
        if (v) { ID3D11ShaderResourceView_GetResource(v, &r); ID3D11ShaderResourceView_Release(v); }
        unsigned k = 0;
        while (r && k < g_novres && k < 8 && g_ovres[k] != r) k++;
        if (r && (k == g_novres || k == 8)) {   // new (a new battle makes new ones: the oldest goes)
            unsigned n = g_novres++ % 8;
            if (g_ovres[n]) ID3D11Resource_Release(g_ovres[n]);
            g_ovres[n] = r;   // the reference is kept: a freed texture's address must not come back as another one
            g_ovkind[n] = kind;
            g_log("DLSS: water %s texture found (slot t%u) - drawn into %s", kind == 1 ? "overlay" : "reflection", s,
                  kind == 1 ? "at full size" : "in the bottom-left corner below native size");
        } else if (r) ID3D11Resource_Release(r);
    }
}

static HRESULT STDMETHODCALLTYPE w_cvs(ID3D11Device *d, const void *bc, SIZE_T len, ID3D11ClassLinkage *cl, ID3D11VertexShader **out) {
    HRESULT hr = r_cvs(d, bc, len, cl, out);
    if (SUCCEEDED(hr) && out && *out && bc) {
        LONG i = InterlockedIncrement(&g_nmpvs) - 1;   // the game creates shaders on loader threads too
        if (i < 4096) {
            int anim = bc_has(bc, len, "variant_animation_size");
            unsigned long long sum = mp_sum(bc, len);
            g_mpvs[i].sum = sum;
            g_mpvs[i].anim = anim;
            g_mpvs[i].ocean = bc_has(bc, len, "g_swell_uv_scale");   // ocean.hlsl vs30_main (naval overlays, below)
            // the game's animated main-view shaders (G1; r2skintest: our skinning is bit-identical to these three)
            g_mpvs[i].type = sum == 0xa779cfb30091fd73ull || sum == g_anim_sum_test[0] ? 0
                           : sum == 0x8dc6a79851998275ull || sum == 0x16ba4ce7298fd83full || sum == g_anim_sum_test[1] ? 1 : -1;
            g_mpvs[i].vs = *out;
            for (unsigned h = ((unsigned)(UINT_PTR)*out >> 4) & 8191, k = 0; k < 8192; h = (h + 1) & 8191, k++)
                if (!InterlockedCompareExchange(&g_mph[h], i + 1, 0)) break;   // shortcut: a freed shader's entry stays (4096 at most)
            if (anim && g_mp) {   // the probe saves the animated ones
                wchar_t p[MAX_PATH];
                _snwprintf(p, MAX_PATH, L"%ls\\motion_vs_%016llx.bin", g_dir, g_mpvs[i].sum);
                FILE *f = _wfopen(p, L"wb");
                if (f) { fwrite(bc, 1, len, f); fclose(f); }
            }
        }
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE w_cil(ID3D11Device *d, const D3D11_INPUT_ELEMENT_DESC *e, UINT n, const void *bc, SIZE_T len, ID3D11InputLayout **out) {
    HRESULT hr = r_cil(d, e, n, bc, len, out);
    if (SUCCEEDED(hr) && out && *out && e && n <= 16) {
        LONG i = InterlockedIncrement(&g_nmpil) - 1;
        if (i < 1024) {
            MpIl *l = &g_mpil[i];
            l->n = n;
            memcpy(l->e, e, n * sizeof *e);
            for (UINT k = 0; k < n; k++) {
                snprintf(l->name[k], 24, "%s", e[k].SemanticName ? e[k].SemanticName : "");
                l->e[k].SemanticName = NULL;
            }
            l->il = *out;
        }
    }
    return hr;
}
static void patch_slot(void **slot, void *to);
static void mp_device_hooks(void) {   // once, on the game's device (all devices of a class share the table)
    if (!(g_mp || g_live) || !g_dev || r_cvs) return;   // also for the motion pass (bridge armed)
    void **vt = *(void ***)g_dev;
    void **s = &vt[offsetof(ID3D11DeviceVtbl, CreateVertexShader) / sizeof(void *)];
    r_cvs = (cvs_fn)*s;
    patch_slot(s, (void *)w_cvs);
    s = &vt[offsetof(ID3D11DeviceVtbl, CreateInputLayout) / sizeof(void *)];
    r_cil = (cil_fn)*s;
    patch_slot(s, (void *)w_cil);
    s = &vt[offsetof(ID3D11DeviceVtbl, CreatePixelShader) / sizeof(void *)];
    r_cps = (cps_fn)*s;
    patch_slot(s, (void *)w_cps);
    if (g_mp) g_log("motion probe on: vertex shaders and input layouts watched; captures battle and campaign frames by itself");
}
static void STDMETHODCALLTYPE w_vsset(ID3D11DeviceContext *c, ID3D11VertexShader *vs, ID3D11ClassInstance *const *ci, UINT n);
static void mp_chunk(FILE *f, const char *tag, const void *a, unsigned na, const void *b, unsigned nb) {
    unsigned h[2];
    memcpy(&h[0], tag, 4);
    h[1] = na + nb;
    fwrite(h, 4, 2, f);
    if (na) fwrite(a, 1, na, f);
    if (nb) fwrite(b, 1, nb, f);
}
static int mp_seen(const void *p, int capture) {   // written already in this frame / this capture? (marks it written)
    const void **w = capture ? g_mp_ws : g_mp_wf;
    unsigned *n = capture ? &g_mp_nws : &g_mp_nwf, cap = capture ? 4096 : 2048;
    for (unsigned i = 0; i < *n; i++) if (w[i] == p) return 1;
    if (*n < cap) w[(*n)++] = p;
    return 0;
}
// a buffer's bytes [off, off + n) through a staging copy (n 0 = all) as a chunk: u64 id, u32 offset, D3D11_BUFFER_DESC, data
static void mp_buffer(FILE *f, const char *tag, ID3D11Buffer *b, UINT off, UINT n) {
    D3D11_BUFFER_DESC d;
    ID3D11Buffer_GetDesc(b, &d);
    if (!n) { off = 0; n = d.ByteWidth; }
    if (off >= d.ByteWidth) return;
    if (off + n > d.ByteWidth) n = d.ByteWidth - off;
    D3D11_BUFFER_DESC s = {n, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
    ID3D11Buffer *st = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &s, NULL, &st))) return;
    D3D11_BOX box = {off, 0, 0, off + n, 1, 1};
    ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)st, 0, 0, 0, 0, (ID3D11Resource *)b, 0, &box);
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
        unsigned long long id = (UINT_PTR)b;
        unsigned hdr[2 + 6] = {(unsigned)id, (unsigned)(id >> 32), off, d.ByteWidth, d.Usage, d.BindFlags, d.CPUAccessFlags, d.StructureByteStride};
        mp_chunk(f, tag, hdr, sizeof hdr, m.pData, n);
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)st, 0);
    }
    ID3D11Buffer_Release(st);
}
// a 2D texture (all mips of slice 0, rows packed) as a chunk: u64 id, D3D11_TEXTURE2D_DESC, per mip: u32 row bytes, rows, data
static void mp_texture(FILE *f, ID3D11Resource *r) {
    D3D11_RESOURCE_DIMENSION dim;
    ID3D11Resource_GetType(r, &dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return;
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)r, &d);
    D3D11_TEXTURE2D_DESC s = d;
    s.Usage = D3D11_USAGE_STAGING; s.BindFlags = 0; s.CPUAccessFlags = D3D11_CPU_ACCESS_READ; s.MiscFlags = 0; s.ArraySize = 1;
    ID3D11Texture2D *st = NULL;
    if (d.SampleDesc.Count != 1 || FAILED(ID3D11Device_CreateTexture2D(g_dev, &s, NULL, &st))) return;
    for (UINT mip = 0; mip < d.MipLevels; mip++)
        ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)st, mip, 0, 0, 0, r, mip, NULL);
    unsigned long long id = (UINT_PTR)r;
    unsigned hdr[2] = {(unsigned)id, (unsigned)(id >> 32)};
    // the size first: every mip's rows
    unsigned total = 0;
    D3D11_MAPPED_SUBRESOURCE m[16];
    UINT nm = d.MipLevels < 16 ? d.MipLevels : 16;
    for (UINT mip = 0; mip < nm; mip++) {
        if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)st, mip, D3D11_MAP_READ, 0, &m[mip]))) { nm = mip; break; }
        UINT h = d.Height >> mip ? d.Height >> mip : 1;
        total += 8 + m[mip].RowPitch * h;
    }
    unsigned h2[2] = {0, 0};
    unsigned head = sizeof hdr + sizeof d + 4;
    unsigned tag_size[2];
    memcpy(&tag_size[0], "TEXR", 4);
    tag_size[1] = head + total;
    fwrite(tag_size, 4, 2, f);
    fwrite(hdr, 4, 2, f);
    fwrite(&d, sizeof d, 1, f);
    fwrite(&nm, 4, 1, f);
    for (UINT mip = 0; mip < nm; mip++) {
        UINT h = d.Height >> mip ? d.Height >> mip : 1;
        h2[0] = m[mip].RowPitch; h2[1] = h;
        fwrite(h2, 4, 2, f);
        fwrite(m[mip].pData, 1, (size_t)m[mip].RowPitch * h, f);
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)st, mip);
    }
    ID3D11Texture2D_Release(st);
}
static MpVs *mp_vs_of(const void *vs) {   // through the hash table (VSSetShader runs hundreds of times a frame)
    for (unsigned h = ((unsigned)(UINT_PTR)vs >> 4) & 8191, k = 0; k < 8192 && g_mph[h]; h = (h + 1) & 8191, k++)
        if (g_mpvs[g_mph[h] - 1].vs == vs) return &g_mpvs[g_mph[h] - 1];
    return NULL;
}
// at every draw of the game's context with the scene depth bound (bridge running)
static void mp_draw(ID3D11DeviceContext *c, int kind, UINT count, UINT inst, UINT start, INT base, UINT sinst) {
    MpVs *v = g_cur_mpvs;
    if (!v) return;
    v->draws++;
    v->insts += inst;
    if (!v->anim) return;
    g_mp_frame_anim++;
    if (!g_mpf) return;
    // the draw: kind, args, VS checksum, input layout, vertex buffers, index buffer
    ID3D11InputLayout *il = NULL;
    ID3D11DeviceContext_IAGetInputLayout(c, &il);
    LONG nl = g_nmpil < 1024 ? g_nmpil : 1024;
    MpIl *l = NULL;
    for (LONG i = nl; i-- > 0 && !l;) if (g_mpil[i].il == il) l = &g_mpil[i];
    if (l && !mp_seen(il, 0)) {   // LAYT: u64 id, n, n x (name[24], index, format, slot, offset, class, step)
        unsigned char b[8 + 4 + 16 * 48];
        unsigned long long id = (UINT_PTR)il;
        unsigned *u = (unsigned *)b;
        u[0] = (unsigned)id; u[1] = (unsigned)(id >> 32); u[2] = l->n;
        for (UINT k = 0; k < l->n; k++) {
            unsigned char *e = b + 12 + k * 48;
            memcpy(e, l->name[k], 24);
            unsigned x[6] = {l->e[k].SemanticIndex, l->e[k].Format, l->e[k].InputSlot, l->e[k].AlignedByteOffset, l->e[k].InputSlotClass,
                             l->e[k].InstanceDataStepRate};
            memcpy(e + 24, x, 24);
        }
        mp_chunk(g_mpf, "LAYT", b, 12 + l->n * 48, NULL, 0);
    }
    ID3D11Buffer *vb[4] = {0}, *ib = NULL;
    UINT str[4] = {0}, off[4] = {0}, ioff = 0;
    DXGI_FORMAT ifmt = DXGI_FORMAT_UNKNOWN;
    ID3D11DeviceContext_IAGetVertexBuffers(c, 0, 4, vb, str, off);
    ID3D11DeviceContext_IAGetIndexBuffer(c, &ib, &ifmt, &ioff);
    unsigned long long ilid = (UINT_PTR)il, ibid = (UINT_PTR)ib;
    unsigned d[32] = {(unsigned)kind, count, inst, start, (unsigned)base, sinst, (unsigned)v->sum, (unsigned)(v->sum >> 32),
                      (unsigned)ilid, (unsigned)(ilid >> 32), (unsigned)ibid, (unsigned)(ibid >> 32), ifmt, ioff};
    for (int s = 0; s < 4; s++) {
        unsigned long long id = (UINT_PTR)vb[s];
        d[14 + s * 4] = (unsigned)id; d[15 + s * 4] = (unsigned)(id >> 32); d[16 + s * 4] = str[s]; d[17 + s * 4] = off[s];
    }
    mp_chunk(g_mpf, "DRAW", d, sizeof d, NULL, 0);
    // instance records: the per-instance slots' range for this draw; meshes: the per-vertex slots and the index buffer, once
    for (int s = 0; s < 4; s++) {
        if (!vb[s]) continue;
        int per_inst = 0;
        for (UINT k = 0; l && k < l->n; k++) per_inst |= l->e[k].InputSlot == (UINT)s && l->e[k].InputSlotClass == D3D11_INPUT_PER_INSTANCE_DATA;
        if (per_inst) mp_buffer(g_mpf, "INST", vb[s], off[s] + sinst * str[s], inst * str[s]);
        else if (!mp_seen(vb[s], 1)) mp_buffer(g_mpmesh, "MESH", vb[s], 0, 0);
    }
    if (ib && !mp_seen(ib, 1)) mp_buffer(g_mpmesh, "MESH", ib, 0, 0);
    // VS constant buffers (camera, animation size, ...) and textures (the animation texture: each once per frame)
    ID3D11Buffer *cb[4] = {0};
    ID3D11DeviceContext_VSGetConstantBuffers(c, 0, 4, cb);
    for (int s = 0; s < 4; s++) if (cb[s]) mp_buffer(g_mpf, s == 0 ? "CBV0" : s == 1 ? "CBV1" : s == 2 ? "CBV2" : "CBV3", cb[s], 0, 0);
    ID3D11ShaderResourceView *sr[4] = {0};
    ID3D11DeviceContext_VSGetShaderResources(c, 0, 4, sr);
    for (int s = 0; s < 4; s++) {
        if (!sr[s]) continue;
        ID3D11Resource *r = NULL;
        ID3D11ShaderResourceView_GetResource(sr[s], &r);
        unsigned long long id = (UINT_PTR)r;
        unsigned t[3] = {(unsigned)s, (unsigned)id, (unsigned)(id >> 32)};
        mp_chunk(g_mpf, "VSRV", t, sizeof t, NULL, 0);
        if (r && !mp_seen(r, 0)) mp_texture(g_mpf, r);
        if (r) ID3D11Resource_Release(r);
        ID3D11ShaderResourceView_Release(sr[s]);
    }
    for (int s = 0; s < 4; s++) { if (vb[s]) ID3D11Buffer_Release(vb[s]); if (cb[s]) ID3D11Buffer_Release(cb[s]); }
    if (ib) ID3D11Buffer_Release(ib);
    if (il) ID3D11InputLayout_Release(il);
}
static void mp_map(ID3D11Resource *r, D3D11_MAP t) {   // Maps of vertex buffers in a capture frame
    D3D11_RESOURCE_DIMENSION dim;
    ID3D11Resource_GetType(r, &dim);
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return;
    D3D11_BUFFER_DESC d;
    ID3D11Buffer_GetDesc((ID3D11Buffer *)r, &d);
    if (!(d.BindFlags & D3D11_BIND_VERTEX_BUFFER)) return;
    for (unsigned i = 0; i < g_nmpmaps; i++)
        if (g_mpmaps[i].buf == r && g_mpmaps[i].type == (UINT)t) { g_mpmaps[i].n++; return; }
    if (g_nmpmaps < 256) { MpMap m = {r, (UINT)t, d.ByteWidth, 1}; g_mpmaps[g_nmpmaps++] = m; }
}
// once per Present: ends a capture frame, starts the next one; the trigger
static void mp_frame(void) {
    if (!g_mp) return;
    if (g_mpf) {   // this capture frame ends: camera, Maps, the draws by vertex shader
        mp_chunk(g_mpf, "CAMB", g_cam_block, sizeof g_cam_block, NULL, 0);
        for (unsigned i = 0; i < g_nmpmaps; i++) {
            unsigned long long id = (UINT_PTR)g_mpmaps[i].buf;
            unsigned m[5] = {(unsigned)id, (unsigned)(id >> 32), g_mpmaps[i].type, g_mpmaps[i].size, g_mpmaps[i].n};
            mp_chunk(g_mpf, "MAPS", m, sizeof m, NULL, 0);
        }
        LONG n = g_nmpvs < 4096 ? g_nmpvs : 4096;
        for (LONG i = 0; i < n; i++)
            if (g_mpvs[i].draws) {
                unsigned s[5] = {(unsigned)g_mpvs[i].sum, (unsigned)(g_mpvs[i].sum >> 32), (unsigned)g_mpvs[i].anim, g_mpvs[i].draws, g_mpvs[i].insts};
                mp_chunk(g_mpf, "VSUM", s, sizeof s, NULL, 0);
            }
        fclose(g_mpf);
        g_mpf = NULL;
        g_log("motion probe: %s frame %d written (%u animated draws)", g_mp_kind ? "campaign" : "battle", g_mp_k, g_mp_frame_anim);
        if (!--g_mp_left) {
            g_mp_done[g_mp_kind] = 1;
            if (g_mpmesh) { fclose(g_mpmesh); g_mpmesh = NULL; }
        }
    }
    LONG n = g_nmpvs < 4096 ? g_nmpvs : 4096;
    for (LONG i = 0; i < n; i++) g_mpvs[i].draws = g_mpvs[i].insts = 0;
    g_nmpmaps = 0;
    g_mp_nwf = 0;   // textures and layouts again in each frame's file
    // trigger: animated models drawn this frame in the main view, for 20 s (fgbridge_on.txt "motionprobe0": at once)
    int kind = g_cam_block[101] > 2000 ? 0 : 1;   // camera far plane: battle 13000, campaign map 500 (captures)
    DWORD now = GetTickCount();
    if (g_mp_frame_anim && !g_mp_done[kind] && !g_mp_left) {
        if (!g_mp_seen[kind]) g_mp_seen[kind] = now;
        if (now - g_mp_seen[kind] >= (g_mp == 2 ? 0u : 20000u)) {
            g_mp_kind = kind;
            g_mp_left = 3;
            g_mp_k = 0;
            wchar_t p[MAX_PATH];
            _snwprintf(p, MAX_PATH, L"%ls\\motion_%ls_mesh.bin", g_dir, kind ? L"campaign" : L"battle");
            g_mpmesh = _wfopen(p, L"wb");
            g_mp_nws = 0;
            g_armed = 1;   // and a bridge capture (targets, viewports, pictures) of the first frame
        }
    }
    g_mp_frame_anim = 0;
    if (g_mp_left && g_mpmesh) {
        wchar_t p[MAX_PATH];
        _snwprintf(p, MAX_PATH, L"%ls\\motion_%ls_%d.bin", g_dir, g_mp_kind ? L"campaign" : L"battle", g_mp_k++);
        g_mpf = _wfopen(p, L"wb");
        if (g_mpf) mp_chunk(g_mpf, "FRAM", &g_frame, 4, NULL, 0);
    }
}

// capture: the shaders (DXBC checksums: the file in data.pack) and the pixel shader's textures at each draw, logged when
// they change (what the banners and the UI read: below native size a screen-sized texture made earlier in the frame
// still holds the shrunk scene; what the water reflection's passes read)
// compute dispatches (depth of field's blurs: the packs' only compute shaders): what they read and write, in captures
static void STDMETHODCALLTYPE w_disp(ID3D11DeviceContext *c, UINT x, UINT y, UINT z) { if (r2m_internal) { ORIG(disp_fn, S_DISP, c)(c, x, y, z); return; }
    if (g_capturing && g_cap && c == g_ctx) {
        ID3D11ComputeShader *cs = NULL;
        ID3D11ShaderResourceView *sv[4] = {0};
        ID3D11UnorderedAccessView *uv[2] = {0};
        ID3D11DeviceContext_CSGetShader(c, &cs, NULL, NULL);
        ID3D11DeviceContext_CSGetShaderResources(c, 0, 4, sv);
        ID3D11DeviceContext_CSGetUnorderedAccessViews(c, 0, 2, uv);
        fprintf(g_cap, "    dispatch after draw %u: cs %p, groups %u x %u x %u", g_draws, (void *)cs, x, y, z);
        char t[160];
        for (int i = 0; i < 4; i++) if (sv[i]) desc_of((ID3D11View *)sv[i], t, sizeof t, NULL), fprintf(g_cap, " t%d [%s]", i, t), ID3D11ShaderResourceView_Release(sv[i]);
        for (int i = 0; i < 2; i++) if (uv[i]) desc_of((ID3D11View *)uv[i], t, sizeof t, NULL), fprintf(g_cap, " u%d [%s]", i, t), ID3D11UnorderedAccessView_Release(uv[i]);
        fprintf(g_cap, "\n");
        if (cs) ID3D11ComputeShader_Release(cs);
    }
    ORIG(disp_fn, S_DISP, c)(c, x, y, z);
}
static void cap_draw_srvs(ID3D11DeviceContext *c) {
    if (!g_capturing || !g_cap || c != g_ctx) return;
    static const void *last_vsh, *last_ps;
    ID3D11PixelShader *ps = NULL;
    ID3D11DeviceContext_PSGetShader(c, &ps, NULL, NULL);
    if (ps) ID3D11PixelShader_Release(ps);
    if (ps != last_ps || g_cur_vs != last_vsh) {
        last_ps = ps, last_vsh = g_cur_vs;
        unsigned long long pss = 0;
        for (LONG i = g_ncapps < 8192 ? g_ncapps : 8192; ps && i-- > 0;) if (g_capps[i].ps == ps) { pss = g_capps[i].sum; break; }
        fprintf(g_cap, "    draw %u shaders: vs %016llx ps %016llx\n", g_draws, g_cur_mpvs ? g_cur_mpvs->sum : 0, pss);
    }
    static ID3D11ShaderResourceView *last[16];
    ID3D11ShaderResourceView *s[16] = {0};
    ID3D11DeviceContext_PSGetShaderResources(c, 0, 16, s);
    if (memcmp(s, last, sizeof s)) {
        memcpy(last, s, sizeof s);
        fprintf(g_cap, "    draw %u reads:", g_draws);
        for (int i = 0; i < 16; i++) {
            if (!s[i]) continue;
            char t[160];
            desc_of((ID3D11View *)s[i], t, sizeof t, NULL);
            fprintf(g_cap, " t%d [%s]", i, t);
        }
        fprintf(g_cap, "\n");
    }
    for (int i = 0; i < 16; i++) if (s[i]) ID3D11ShaderResourceView_Release(s[i]);
    // and its samplers (filter, address u/v/w, mip bias, mip range: the texture mip bias below native size shows here)
    static ID3D11SamplerState *last_smp[16];
    ID3D11SamplerState *smp[16] = {0};
    ID3D11DeviceContext_PSGetSamplers(c, 0, 16, smp);
    if (memcmp(smp, last_smp, sizeof smp)) {
        memcpy(last_smp, smp, sizeof smp);
        fprintf(g_cap, "    draw %u samplers:", g_draws);
        for (int i = 0; i < 16; i++) {
            if (!smp[i]) continue;
            D3D11_SAMPLER_DESC sd;
            ID3D11SamplerState_GetDesc(smp[i], &sd);
            fprintf(g_cap, " s%d [%p filter 0x%x address %u/%u/%u bias %.2f mips %.0f..%.0f]", i, (void *)smp[i], sd.Filter, sd.AddressU,
                    sd.AddressV, sd.AddressW, sd.MipLODBias, sd.MinLOD, sd.MaxLOD);
        }
        fprintf(g_cap, "\n");
    }
    for (int i = 0; i < 16; i++) if (smp[i]) ID3D11SamplerState_Release(smp[i]);
    // the vertex shader's too (banners may test their visibility against a depth image there)
    static ID3D11ShaderResourceView *last_vs[8];
    ID3D11ShaderResourceView *vsr[8] = {0};
    ID3D11DeviceContext_VSGetShaderResources(c, 0, 8, vsr);
    if (memcmp(vsr, last_vs, sizeof vsr)) {
        memcpy(last_vs, vsr, sizeof vsr);
        fprintf(g_cap, "    draw %u VS reads:", g_draws);
        for (int i = 0; i < 8; i++) {
            if (!vsr[i]) continue;
            char t[160];
            desc_of((ID3D11View *)vsr[i], t, sizeof t, NULL);
            fprintf(g_cap, " t%d [%s]", i, t);
        }
        fprintf(g_cap, "\n");
    }
    for (int i = 0; i < 8; i++) if (vsr[i]) ID3D11ShaderResourceView_Release(vsr[i]);
    // and the depth / stencil state (below native size the stencil keeps the shrunk scene: only the depth is put back)
    static ID3D11DepthStencilState *last_dss;
    static UINT last_ref = ~0u;
    ID3D11DepthStencilState *dss = NULL;
    UINT ref = 0;
    ID3D11DeviceContext_OMGetDepthStencilState(c, &dss, &ref);
    D3D11_DEPTH_STENCIL_DESC d = {TRUE, D3D11_DEPTH_WRITE_MASK_ALL, D3D11_COMPARISON_LESS, FALSE, 0xFF, 0xFF, {1, 1, 1, D3D11_COMPARISON_ALWAYS}, {1, 1, 1, D3D11_COMPARISON_ALWAYS}};
    if (dss) ID3D11DepthStencilState_GetDesc(dss, &d);
    static int dumped_cap;   // the depth the first depth-tested draw after the HUD-less point tests against (the banners)
    if (dumped_cap != g_captures && d.DepthEnable && d.DepthFunc != D3D11_COMPARISON_ALWAYS) {
        dumped_cap = g_captures;
        dump_depth("first depth-tested draw after the HUD-less point");
    }
    if (dss != last_dss || ref != last_ref) {
        last_dss = dss;
        last_ref = ref;
        fprintf(g_cap, "    draw %u depth/stencil: depth %d write %d func %d; stencil %d read 0x%02x write 0x%02x ref %u front func %d pass %d "
                       "fail %d depthfail %d\n", g_draws, d.DepthEnable, d.DepthWriteMask, d.DepthFunc, d.StencilEnable, d.StencilReadMask,
                d.StencilWriteMask, ref, d.FrontFace.StencilFunc, d.FrontFace.StencilPassOp, d.FrontFace.StencilFailOp, d.FrontFace.StencilDepthFailOp);
    }
    if (dss) ID3D11DepthStencilState_Release(dss);
    // the rasterizer state (depth bias, depth clip, scissor) and the pixel shaders' cb0 (the game's camera or our copy)
    static ID3D11RasterizerState *last_rs;
    ID3D11RasterizerState *rs = NULL;
    ID3D11DeviceContext_RSGetState(c, &rs);
    if (rs != last_rs) {
        last_rs = rs;
        D3D11_RASTERIZER_DESC r = {D3D11_FILL_SOLID, D3D11_CULL_BACK, FALSE, 0, 0, 0, TRUE, FALSE, FALSE, FALSE};
        if (rs) ID3D11RasterizerState_GetDesc(rs, &r);
        D3D11_RECT sc = {0, 0, 0, 0};
        UINT nsc = 1;
        ID3D11DeviceContext_RSGetScissorRects(c, &nsc, &sc);
        fprintf(g_cap, "    draw %u rasterizer: cull %d bias %d clamp %g slope %g depthclip %d scissor %d (%ld %ld %ld %ld)\n", g_draws, r.CullMode,
                r.DepthBias, r.DepthBiasClamp, r.SlopeScaledDepthBias, r.DepthClipEnable, r.ScissorEnable, sc.left, sc.top, sc.right, sc.bottom);
    }
    if (rs) ID3D11RasterizerState_Release(rs);
    static ID3D11Buffer *last_pcb;
    ID3D11Buffer *pcb = NULL;
    ID3D11DeviceContext_PSGetConstantBuffers(c, 0, 1, &pcb);
    if (pcb != last_pcb) {
        last_pcb = pcb;
        int ours = is_cam_copy(pcb);
        fprintf(g_cap, "    draw %u pixel shader cb0 @%p%s\n", g_draws, (void *)pcb, ours ? " (the bridge's patched camera copy)" : "");
    }
    if (pcb) ID3D11Buffer_Release(pcb);
}
// the motion probe sees the main view's draws: the scene depth bound, before the HUD-less point
// the motion pass records the animated main-view draws (the game's three skinned shaders) with the game's input layout
static void motion_draw(ID3D11DeviceContext *c, int indexed, UINT count, UINT inst, UINT start, INT base, UINT sinst) {
    ID3D11InputLayout *il = NULL;
    ID3D11DeviceContext_IAGetInputLayout(c, &il);
    LONG nl = g_nmpil < 1024 ? g_nmpil : 1024;
    MpIl *l = NULL;
    for (LONG i = nl; i-- > 0 && !l;) if (g_mpil[i].il == il) l = &g_mpil[i];
    if (l) {
        D3D11_INPUT_ELEMENT_DESC e[16];
        memcpy(e, l->e, l->n * sizeof *e);
        for (UINT k = 0; k < l->n; k++) e[k].SemanticName = l->name[k];
        r2m_draw(c, g_cur_mpvs->type, il, e, l->n, indexed, count, inst, start, base, sinst);
    }
    if (il) ID3D11InputLayout_Release(il);
}
#define MP_DRAW(kind, count, inst, start, base, sinst)                                                                        \
    if (c == g_ctx && g_jit_cur && g_jit_frame != g_frame) g_jit_frame = g_frame, ps_cb_apply(c);                            \
    if (c == g_ctx && g_live_state == 1 && g_lv_scene && g_hud_frame != g_frame) {                                           \
        if (g_mp) mp_draw(c, kind, count, inst, start, base, sinst);                                                         \
        if (g_motion && g_cur_mpvs && g_cur_mpvs->type >= 0) motion_draw(c, (kind) & 1, count, inst, start, base, sinst);    \
        if ((g_cur_psf & PSF_WATER) || (g_cur_mpvs && g_cur_mpvs->ocean)) overlay_learn(c);                                  \
    }                                                                                                                        \
    if (c == g_ctx && g_live_state == 1 && g_rt_overlay == 2) ssr_begin(c);                                                 \
    if (c == g_ctx && g_live_state == 1 && (g_cur_psf & (PSF_TM | PSF_CAM))) tm_begin(c), cs_begin(c);
static void ssr_begin(ID3D11DeviceContext *c), ssr_end(ID3D11DeviceContext *c), tm_begin(ID3D11DeviceContext *c), tm_end(ID3D11DeviceContext *c);
static void cs_begin(ID3D11DeviceContext *c), cs_end(ID3D11DeviceContext *c), ps_cb_apply(ID3D11DeviceContext *c);
static int g_jit_cur;          // (below, with jit_apply)
static unsigned g_jit_frame;
// the glue's own time at the game's draws and sampler binds, every 8th frame (the 30 s report: what the checks per draw cost)
static int g_glue_meas;
static unsigned g_glue_frames, g_glue_draws, g_glue_sbinds;
static LONGLONG g_glue_t, g_glue_ts;
#define GLUE_T0 LARGE_INTEGER q0, q1; int gm = g_glue_meas && c == g_ctx; if (gm) QueryPerformanceCounter(&q0);
#define GLUE_T1(acc) if (gm) { QueryPerformanceCounter(&q1); acc += q1.QuadPart - q0.QuadPart; }
static void STDMETHODCALLTYPE w_dip(ID3D11DeviceContext *c, UINT a, UINT b, INT d) { if (r2m_internal) { ORIG(dip_fn, S_DIP, c)(c, a, b, d); return; } GLUE_T0 if (c == g_ctx) g_draws++, g_set_draws++, g_lv_draws++; cap_draw_srvs(c); MP_DRAW(1, a, 1, b, d, 0); GLUE_T1(g_glue_t) g_glue_draws += gm; ORIG(dip_fn, S_DIP, c)(c, a, b, d); if (gm) QueryPerformanceCounter(&q0); ssr_end(c); tm_end(c); cs_end(c); GLUE_T1(g_glue_t) }
static void STDMETHODCALLTYPE w_d(ID3D11DeviceContext *c, UINT a, UINT b) { if (r2m_internal) { ORIG(d_fn, S_D, c)(c, a, b); return; } GLUE_T0 if (c == g_ctx) g_draws++, g_set_draws++, g_lv_draws++; cap_draw_srvs(c); MP_DRAW(0, a, 1, b, 0, 0); GLUE_T1(g_glue_t) g_glue_draws += gm; ORIG(d_fn, S_D, c)(c, a, b); if (gm) QueryPerformanceCounter(&q0); ssr_end(c); tm_end(c); cs_end(c); GLUE_T1(g_glue_t) }
static void STDMETHODCALLTYPE w_dii(ID3D11DeviceContext *c, UINT a, UINT b, UINT d, INT e, UINT f) { if (r2m_internal) { ORIG(dii_fn, S_DII, c)(c, a, b, d, e, f); return; } GLUE_T0 if (c == g_ctx) g_draws++, g_set_draws++, g_lv_draws++; cap_draw_srvs(c); MP_DRAW(3, a, b, d, e, f); GLUE_T1(g_glue_t) g_glue_draws += gm; ORIG(dii_fn, S_DII, c)(c, a, b, d, e, f); if (gm) QueryPerformanceCounter(&q0); ssr_end(c); tm_end(c); cs_end(c); GLUE_T1(g_glue_t) }
static void STDMETHODCALLTYPE w_di(ID3D11DeviceContext *c, UINT a, UINT b, UINT d, UINT e) { if (r2m_internal) { ORIG(di_fn, S_DI, c)(c, a, b, d, e); return; } GLUE_T0 if (c == g_ctx) g_draws++, g_set_draws++, g_lv_draws++; cap_draw_srvs(c); MP_DRAW(2, a, b, d, 0, e); GLUE_T1(g_glue_t) g_glue_draws += gm; ORIG(di_fn, S_DI, c)(c, a, b, d, e); if (gm) QueryPerformanceCounter(&q0); ssr_end(c); tm_end(c); cs_end(c); GLUE_T1(g_glue_t) }
static void STDMETHODCALLTYPE w_vsset(ID3D11DeviceContext *c, ID3D11VertexShader *vs, ID3D11ClassInstance *const *ci, UINT n) {
    if (r2m_internal) { ORIG(vsset_fn, S_VSSET, c)(c, vs, ci, n); return; }
    if ((g_mp || g_live) && c == g_ctx && vs != g_cur_vs) { g_cur_vs = vs; g_cur_mpvs = mp_vs_of(vs); }
    ORIG(vsset_fn, S_VSSET, c)(c, vs, ci, n);
}

// ---- DLSS Super Resolution (Attila AGENT_LOG 96 / 103, same rules). DLAA: the sub-pixel jitter goes into the viewport,
// which shifts everything rasterised like a jittered projection while the camera block (and so the motion vectors) stays
// unjittered. Jittered: viewports covering a whole screen-sized target while the scene depth is bound and depth testing
// is on, from the frame start to the HUD-less point. Not jittered: shadow maps, half-size passes, full-screen passes
// without depth test, post-processing, the UI. Below native size (Quality .. Ultra Performance), from the frame start to
// the HUD-less point every viewport covering a whole target of the screen's size or 1/2, 1/4, 1/8 of it is shrunk by the
// render scale, so the scene lands in the top-left render_w x render_h of each; DLSS upscales that corner and the UI is
// drawn at full size afterwards. The game reads back the viewports it set.
static int g_jit_on, g_scene_last;       // this frame is jittered / scaled (until the HUD-less point); the last frame had a scene
static unsigned g_jit_frame = ~0u;       // the frame whose scene draws have been jittered (from then on pixel shaders see it)
static int g_jit_cur;                    // the viewports applied now are jittered (set by jit_apply, taken at the next draw)
static float g_jx, g_jy;
static D3D11_VIEWPORT g_gvp[16], g_avp[16];   // the game's viewports as it set them; what was applied last
static UINT g_ngvp;
static int g_dss_depth = 1;
static ID3D11DepthStencilState *g_last_dss;
static unsigned g_jit_frames, g_jit_vp;
static int screen_part(UINT tw, UINT th) {   // tw x th = screen / k for k = 1, 2, 4, 8
    for (UINT k = 1; k <= 8; k *= 2)
        if (g_live_w % k == 0 && g_live_h % k == 0 && tw == g_live_w / k && th == g_live_h / k) return (int)k;
    return 0;
}

// The tone mapper's first step (hdr_tone_mapper.hlsl ps_downsample_image; user: campaign at Quality "almost looks like the
// shadows get changed", 20-40 % brighter than DLAA): the HDR scene into a 128x128 target at uv = SV_Position *
// g_render_target_recip_size (1 / 128): the whole screen-sized texture. Below native size only the render-size corner
// holds the frame (the rest cleared black), so the black and white points it leads to came out darker and the picture
// brighter. For a tone-mapper pixel shader (only hdr_tone_mapper.hlsl names g_render_target_recip_size) reading a
// screen-sized t0 into a target we do not shrink, its render_target_dims_buffer is swapped for one scaled by render /
// output size for that draw. shortcut: the size is made from the bound target (1 / its size, as the uv above needs at
// native size), not read from the game's buffer.
static ID3D11Buffer *g_tm_cb, *g_tm_game;
static int g_tm_slot = -1;
static unsigned g_tm_draws;
static void tm_begin(ID3D11DeviceContext *c) {
    if (!g_jit_on || g_hud_frame == g_frame || (g_rw == g_live_w && g_rh == g_live_h) || !g_rt_w || !g_rt_h || screen_part(g_rt_w, g_rt_h)) return;
    ID3D11PixelShader *ps = NULL;
    ID3D11DeviceContext_PSGetShader(c, &ps, NULL, NULL);
    unsigned m = 0;
    for (LONG i = g_novps < 64 ? g_novps : 64; ps && i-- > 0;) if (g_ovps[i].ps == ps) m = g_ovps[i].tmcb;
    if (ps) ID3D11PixelShader_Release(ps);
    if (!m) return;
    ID3D11ShaderResourceView *v = NULL;
    ID3D11DeviceContext_PSGetShaderResources(c, 0, 1, &v);
    D3D11_TEXTURE2D_DESC d = {0};
    if (v) {
        ID3D11Resource *r = NULL;
        D3D11_RESOURCE_DIMENSION dim;
        ID3D11ShaderResourceView_GetResource(v, &r);
        ID3D11Resource_GetType(r, &dim);
        if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) ID3D11Texture2D_GetDesc((ID3D11Texture2D *)r, &d);
        ID3D11Resource_Release(r);
        ID3D11ShaderResourceView_Release(v);
    }
    if (d.Width != g_live_w || d.Height != g_live_h) return;   // the scene itself, not one of the small steps after it
    unsigned slot = 0;
    while (!(m >> slot & 1)) slot++;
    if (slot >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT) return;
    if (!g_tm_cb) {
        D3D11_BUFFER_DESC bd = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &g_tm_cb))) { g_tm_cb = NULL; return; }
    }
    float k[4] = {(float)g_rw / g_live_w / g_rt_w, (float)g_rh / g_live_h / g_rt_h, 0, 0};
    r2m_internal = 1;
    ID3D11DeviceContext_UpdateSubresource(c, (ID3D11Resource *)g_tm_cb, 0, NULL, k, 0, 0);
    ID3D11DeviceContext_PSGetConstantBuffers(c, slot, 1, &g_tm_game);
    ID3D11DeviceContext_PSSetConstantBuffers(c, slot, 1, &g_tm_cb);
    r2m_internal = 0;
    g_tm_slot = (int)slot;
    if (!g_tm_draws++) g_log("DLSS: the tone mapper measures the render-size corner of the scene (%ux%u of %ux%u)", g_rw, g_rh, g_live_w, g_live_h);
}
static void tm_end(ID3D11DeviceContext *c) {   // after that draw: the game's buffer back
    if (g_tm_slot < 0) return;
    r2m_internal = 1;
    ID3D11DeviceContext_PSSetConstantBuffers(c, (UINT)g_tm_slot, 1, &g_tm_game);
    r2m_internal = 0;
    if (g_tm_game) ID3D11Buffer_Release(g_tm_game);
    g_tm_game = NULL;
    g_tm_slot = -1;
}

// Pixel shaders below native size (Attila AGENT_LOG 103; Rome 2's point-light shader checked: camera_VS_PS in cb0,
// ndc = (SV_Position + g_vpos_texel_offset) / g_screen_size * 2 - 1, then inv_view_projection): the sizes stay the
// screen's (they also turn pixels into texture coordinates, right as they are with the scene in the top-left), so while
// the scene is scaled, pixel shaders get a copy of the camera block whose matrices see the full-size target as the
// screen: inverse ones M x S, forward ones S^-1 x M (x and y only). Vertex shaders keep the game's block.
typedef void(STDMETHODCALLTYPE *pscb_fn)(ID3D11DeviceContext *, UINT, UINT, ID3D11Buffer *const *);
typedef struct { ID3D11Buffer *game, *ours, *bl; UINT size; int valid, bl_valid; } CamCopy;   // bl: for the bottom-left (reflection)
static CamCopy g_ccopy[8];
static unsigned g_nccopy, g_ccopy_writes;
static ID3D11Buffer *g_ps_cb0, *g_ps_bound;   // the game's pixel shader constant buffer 0 (one reference held), what is bound
static CamCopy *cam_copy(ID3D11Resource *r, int add) {   // r is a camera buffer (is_cam_buffer) when add is set
    for (unsigned i = 0; i < g_nccopy; i++)
        if ((ID3D11Resource *)g_ccopy[i].game == r) return &g_ccopy[i];
    if (!add || g_nccopy == 8) return NULL;   // shortcut: 8 camera buffers at most (Rome 2 uses 1), no reuse of freed ones
    D3D11_BUFFER_DESC d;
    ID3D11Buffer_GetDesc((ID3D11Buffer *)r, &d);
    CamCopy *e = &g_ccopy[g_nccopy];
    D3D11_BUFFER_DESC od = {d.ByteWidth, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &od, NULL, &e->ours))) return NULL;
    ID3D11Buffer_AddRef((ID3D11Buffer *)r);   // a freed buffer's address must not come back as another one
    e->game = (ID3D11Buffer *)r;
    e->size = d.ByteWidth;
    e->valid = e->bl_valid = 0;
    e->bl = NULL;
    g_nccopy++;
    return e;
}
// the camera block as the pixel shaders need it at render size rw x rh (src: the game's block, size bytes), the scene in
// the top-left corner, or the bottom-left (bottom: the ocean's reflection), drawn with the sub-pixel jitter (jx, jy) in
// render pixels (x right, y down). A pixel the shader turns into ndc (SV_Position / screen * 2 - 1, the screen's size)
// is, in the full view, x = sx * ndc + sx - 1 - 2 jx / rw, y = sy * ndc + (1 - sy, or sy - 1 at the bottom) + 2 jy / rh.
// With the jitter in it, the lighting, shadows, fog and every other pass that turns pixels back into positions (Rome 2
// is deferred) sees the geometry where the jittered viewport drew it: without it they were half a pixel off in another
// direction each frame and DLSS blurred them (DLAA with jitter softer than without; user: "DLAA is very blurry")
static void cam_patch(float *f, UINT rw, UINT rh, int bottom, float jx, float jy) {
    float sx = (float)g_live_w / rw, sy = (float)g_live_h / rh;
    float ox = sx - 1 - 2 * jx / rw, oy = (bottom ? sy - 1 : 1 - sy) + 2 * jy / rh;
    for (int m = 0; m < 2; m++) {   // inverse_projection (byte 272), inv_view_projection (336): rows r_i -> r_i * S
        float *r = f + (m ? 84 : 68);
        for (int i = 0; i < 4; i++, r += 4) { r[3] += r[0] * ox + r[1] * oy; r[0] *= sx; r[1] *= sy; }
    }
    for (int m = 0; m < 2; m++) {   // projection (80), view_projection (144): rows x, y -> S^-1 * M
        float *x = f + (m ? 36 : 20), *y = x + 4, *w = x + 12;
        for (int j = 0; j < 4; j++) { x[j] = (x[j] - ox * w[j]) / sx; y[j] = (y[j] - oy * w[j]) / sy; }
    }
}
static void cam_copy_write(ID3D11Resource *r, const void *src) {   // every camera block write while below native size or jittered
    if (g_rw == g_live_w && g_rh == g_live_h && !g_jx && !g_jy) return;
    CamCopy *e = cam_copy(r, 1);
    if (!e) return;
    float f[144];
    memcpy(f, src, e->size < sizeof f ? e->size : sizeof f);
    cam_patch(f, g_rw, g_rh, 0, g_jx, g_jy);
    ORIG(upd_fn, S_UPD, g_ctx)(g_ctx, (ID3D11Resource *)e->ours, 0, NULL, f, 0, 0);
    e->valid = 1;
    g_ccopy_writes++;
    if (!g_novres) return;   // no ocean seen: no reflection to draw
    D3D11_BUFFER_DESC od = {e->size, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    if (!e->bl && FAILED(ID3D11Device_CreateBuffer(g_dev, &od, NULL, &e->bl))) { e->bl = NULL; return; }
    memcpy(f, src, e->size < sizeof f ? e->size : sizeof f);
    cam_patch(f, g_rw, g_rh, 1, 0, 0);   // the reflection pass is not jittered
    ORIG(upd_fn, S_UPD, g_ctx)(g_ctx, (ID3D11Resource *)e->bl, 0, NULL, f, 0, 0);
    e->bl_valid = 1;
}
static int is_cam_copy(ID3D11Buffer *b) {
    for (unsigned i = 0; i < g_nccopy; i++) if (b && (b == g_ccopy[i].ours || b == g_ccopy[i].bl)) return 1;
    return 0;
}
static void ps_cb_apply(ID3D11DeviceContext *c) {
    int scaled = g_rw != g_live_w || g_rh != g_live_h;
    CamCopy *e = g_ps_cb0 && g_jit_on && g_rt_overlay != 1 && (scaled || ((g_jx || g_jy) && g_jit_frame == g_frame)) && screen_part(g_rt_w, g_rt_h)
                     ? cam_copy((ID3D11Resource *)g_ps_cb0, 0) : NULL;
    ID3D11Buffer *want = !e ? g_ps_cb0 : g_rt_overlay == 2 ? (scaled && e->bl_valid ? e->bl : g_ps_cb0) : e->valid ? e->ours : g_ps_cb0;
    if (want != g_ps_bound) { ORIG(pscb_fn, S_PSCB, c)(c, 0, 1, &want); g_ps_bound = want; }
}
static void STDMETHODCALLTYPE w_pscb(ID3D11DeviceContext *c, UINT start, UINT n, ID3D11Buffer *const *b) { if (r2m_internal) { ORIG(pscb_fn, S_PSCB, c)(c, start, n, b); return; }
    if (g_live_state != 1 || c != g_ctx || start != 0 || !n || !b || n > 14) { ORIG(pscb_fn, S_PSCB, c)(c, start, n, b); return; }
    ID3D11Buffer *g = b[0];
    for (unsigned i = 0; i < g_nccopy; i++)
        if (g == g_ccopy[i].ours || (g && g == g_ccopy[i].bl)) g = g_ccopy[i].game;   // our copy, read back by someone saving state: it stands for the game's
    if (g) ID3D11Buffer_AddRef(g);
    if (g_ps_cb0) ID3D11Buffer_Release(g_ps_cb0);
    g_ps_cb0 = g;
    ID3D11Buffer *v[14];
    memcpy(v, b, n * sizeof *v);
    v[0] = g;
    ORIG(pscb_fn, S_PSCB, c)(c, 0, n, v);
    g_ps_bound = g;
    ps_cb_apply(c);
}

static void jit_apply(ID3D11DeviceContext *c, int force) {
    int k = g_jit_on && g_rt_overlay != 1 ? screen_part(g_rt_w, g_rt_h) : 0;
    int jitter = g_jit_on && g_lv_scene && g_dss_depth && k == 1;
    g_jit_cur = jitter && (g_jx || g_jy);
    ps_cb_apply(c);
    if (!g_ngvp) return;
    D3D11_VIEWPORT v[16];
    memcpy(v, g_gvp, g_ngvp * sizeof *v);
    for (UINT i = 0; k && i < g_ngvp; i++) {
        D3D11_VIEWPORT *x = &v[i];
        if (x->TopLeftX != 0 || x->TopLeftY != 0 || x->Width != (float)g_rt_w || x->Height != (float)g_rt_h) continue;
        if (g_rw != g_live_w || g_rh != g_live_h) {
            x->Width = (float)g_rw / k;
            x->Height = (float)g_rh / k;
            if (g_rt_overlay == 2) x->TopLeftY = (float)g_rt_h - x->Height;   // the reflection: bottom-left
        }
        if (jitter) { x->TopLeftX += g_jx; x->TopLeftY += g_jy; g_jit_vp++; }
    }
    if (!force && !memcmp(v, g_avp, g_ngvp * sizeof *v)) return;
    ORIG(rsvp_fn, S_RSVP, c)(c, g_ngvp, v);
    memcpy(g_avp, v, g_ngvp * sizeof *v);
    if (g_capturing && g_cap)
        fprintf(g_cap, "    viewport %u: game (%g %g %g %g depth %g-%g), applied (%g %g %g %g)%s, target %ux%u, scaled %d, draws so far %u\n",
                g_ngvp, g_gvp[0].TopLeftX, g_gvp[0].TopLeftY, g_gvp[0].Width, g_gvp[0].Height, g_gvp[0].MinDepth, g_gvp[0].MaxDepth,
                v[0].TopLeftX, v[0].TopLeftY, v[0].Width, v[0].Height, force ? " (game's call)" : "", g_rt_w, g_rt_h, g_jit_on, g_draws);
}
// Texture detail below native size (mip bias log2(render width / output width): Quality -0.58, Performance -1): the
// game picks mip levels for the render size, so ground and unit textures came out softer than at native. NVIDIA's DLSS
// Programming Guide 3.5 adds a further -1.0 (DLAA too); tried in game it lifted the shadows' darkness at DLAA (mid-dark
// pixels +2.5 of 255 against native: the game's look changed; user), so the sharpening below the output recovers that
// detail instead (DLSS_LOG 45 / 46). While the scene is drawn small, its material samplers (anisotropic or wrapping; not shadow comparison, clamped post-process
// ones) are swapped for copies with that bias added. Only for draws into the scene's targets (screen / k): the shadow
// maps keep the game's (campaign, Quality: the trees' alpha-tested leaves cast lighter shadows with the bias; user: "it
// almost looks like the shadows get changed"). The game's samplers are held (AddRef) so an address is never
// reused for another one; the copies are made again when the render size changes.
typedef void(STDMETHODCALLTYPE *pssamp_fn)(ID3D11DeviceContext *, UINT, UINT, ID3D11SamplerState *const *);
static struct { ID3D11SamplerState *game, *ours; } g_lod[256];
static unsigned g_nlod, g_lod_swaps, g_lod_full;
static float g_lod_bias;   // the bias the copies were made with
static struct { ID3D11SamplerState *in, *out; } g_lodc[256];   // the last answers, by address (cleared with the copies)
static int g_lod_off;      // dev: switched off from the F11 panel's Debug View row (to compare)
static int g_jit_off;      // dev: no sub-pixel jitter (Debug View "No jitter": DLAA's own softness against misalignment)
static int g_jtest;        // dev: what DLSS is told of the jitter (the panel's Jitter Test; the viewports keep the frame's)
static ID3D11SamplerState *lod_sampler(ID3D11SamplerState *s, float bias) {
    if (!s) return s;
    if (bias != g_lod_bias) {   // a new render size: new copies
        for (unsigned i = 0; i < g_nlod; i++) {
            ID3D11SamplerState_Release(g_lod[i].game);
            if (g_lod[i].ours) ID3D11SamplerState_Release(g_lod[i].ours);
        }
        g_nlod = 0;
        g_lod_bias = bias;
        memset(g_lodc, 0, sizeof g_lodc);
    }
    unsigned ch = ((unsigned)(UINT_PTR)s >> 4) & 255;
    if (g_lodc[ch].in == s) return g_lodc[ch].out;
    for (unsigned i = 0; i < g_nlod; i++) {
        if (g_lod[i].game == s) { g_lodc[ch].in = s; return g_lodc[ch].out = g_lod[i].ours ? g_lod[i].ours : s; }
        if (g_lod[i].ours == s) { g_lodc[ch].in = s; return g_lodc[ch].out = s; }   // our copy, read back and set again by someone saving state
    }
    if (g_nlod == 256) { g_lod_full++; return s; }   // shortcut: 256 game samplers at most, the rest keep their bias
    D3D11_SAMPLER_DESC d;
    ID3D11SamplerState_GetDesc(s, &d);
    ID3D11SamplerState *o = NULL;
    if (d.Filter < D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT && (d.Filter == D3D11_FILTER_ANISOTROPIC || d.AddressU == D3D11_TEXTURE_ADDRESS_WRAP)) {
        d.MipLODBias = fmaxf(-16.0f, d.MipLODBias + bias);
        if (FAILED(ID3D11Device_CreateSamplerState(g_dev, &d, &o))) o = NULL;
    }
#ifdef PROFILING
    g_log("texture mip bias: game sampler %p filter 0x%x address %u/%u/%u bias %.2f mips %.0f..%.0f anisotropy %u - %s", (void *)s, d.Filter,
          d.AddressU, d.AddressV, d.AddressW, o ? d.MipLODBias - bias : d.MipLODBias, d.MinLOD, d.MaxLOD, d.MaxAnisotropy, o ? "biased" : "kept");
#endif
    ID3D11SamplerState_AddRef(s);
    g_lod[g_nlod].game = s;
    g_lod[g_nlod++].ours = o;
    return o ? o : s;
}
static void STDMETHODCALLTYPE w_pssamp(ID3D11DeviceContext *c, UINT start, UINT n, ID3D11SamplerState *const *s) { if (r2m_internal) { ORIG(pssamp_fn, S_PSSAMP, c)(c, start, n, s); return; }
    if (g_live_state != 1 || c != g_ctx || !s || !n || n > D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT || !g_frame_scaled || g_hud_frame == g_frame ||
        g_rw >= g_live_w || g_lod_off || !screen_part(g_rt_w, g_rt_h)) { ORIG(pssamp_fn, S_PSSAMP, c)(c, start, n, s); return; }
    GLUE_T0
    float bias = log2f((float)g_rw / g_live_w);
    ID3D11SamplerState *b[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT];
    for (UINT i = 0; i < n; i++) g_lod_swaps += (b[i] = lod_sampler(s[i], bias)) != s[i];
    GLUE_T1(g_glue_ts)
    g_glue_sbinds += gm;
    ORIG(pssamp_fn, S_PSSAMP, c)(c, start, n, b);
}

static void STDMETHODCALLTYPE w_rsvp(ID3D11DeviceContext *c, UINT n, const D3D11_VIEWPORT *v) { if (r2m_internal) { ORIG(rsvp_fn, S_RSVP, c)(c, n, v); return; }
    if (g_live_state != 1 || c != g_ctx || n > 16 || !v) { ORIG(rsvp_fn, S_RSVP, c)(c, n, v); return; }
    g_ngvp = n;
    memcpy(g_gvp, v, n * sizeof *v);
    jit_apply(c, 1);
}
static void STDMETHODCALLTYPE w_rsgetvp(ID3D11DeviceContext *c, UINT *n, D3D11_VIEWPORT *v) { if (r2m_internal) { ORIG(rsgetvp_fn, S_RSGETVP, c)(c, n, v); return; }   // the game reads back what it set
    if (g_live_state != 1 || c != g_ctx || !n || !g_ngvp) { ORIG(rsgetvp_fn, S_RSGETVP, c)(c, n, v); return; }
    if (v) memcpy(v, g_gvp, (*n < g_ngvp ? *n : g_ngvp) * sizeof *v);
    *n = g_ngvp;
}
static void STDMETHODCALLTYPE w_omdss(ID3D11DeviceContext *c, ID3D11DepthStencilState *st, UINT ref) { if (r2m_internal) { ORIG(omdss_fn, S_OMDSS, c)(c, st, ref); return; }
    ORIG(omdss_fn, S_OMDSS, c)(c, st, ref);
    if (g_live_state != 1 || c != g_ctx) return;
    if (st != g_last_dss) {
        g_last_dss = st;
        D3D11_DEPTH_STENCIL_DESC d = {0};
        d.DepthEnable = TRUE;   // NULL state = default: depth test on
        if (st) ID3D11DepthStencilState_GetDesc(st, &d);
        g_dss_depth = d.DepthEnable;
    }
    jit_apply(c, 0);
}
// frame start (after the bridge's Present): this frame's jitter, only after a frame with a scene (not loading screens);
// HUD-less point or Present: the game's own viewports again
static void jit_frame_start(void) {
    g_frame_scaled = 0;
    if (!g_sr) return;
    fgb_jitter(&g_jx, &g_jy);
    if (g_jit_off) { g_jx = g_jy = 0; fgb_set_jitter(0, 0); }
    else if (g_jtest) {
        float tx = g_jx, ty = g_jy;
        if (g_jtest == 1) tx = ty = 0;
        else if (g_jtest == 2) tx = -tx;
        else if (g_jtest == 3) ty = -ty;
        else fgb_jitter_at(g_jtest == 4 ? -1 : 1, &tx, &ty);
        fgb_set_jitter(tx, ty);
    }
    // loading screens and movies have no scene (no HUD-less point): never shrunk, or they would show at the render size
    // (Attila 6.4); a scene frame that was not (the first after loading) goes out at native size without DLSS
    g_jit_on = g_scene_last && (g_jx != 0 || g_jy != 0 || g_rw != g_live_w || g_rh != g_live_h);
    g_frame_scaled = g_jit_on;
    g_jit_frames += g_jit_on;
    jit_apply(g_ctx, 0);
}
static void jit_frame_end(void) {
    if (!g_jit_on) return;
    g_jit_on = 0;
    jit_apply(g_ctx, 0);
}

// The water reflection's ships (21:48 capture): after the main view the game draws one quad into the reflection with
// helpers.hlsl scree_space_reflections_ps, which marches through the scene depth (t1, 3840x2160) and samples the
// half-size scene copy (t0) at full-screen uv (its depth lookup at (u, 1 - v) of its quad, the rest from its own
// view_projection). Below native size both hold the scene in their top-left render-size corner: it read the wrong
// places (a dark rectangle, ships misplaced). So for that draw it gets copies of just that corner, which its uv 0..1
// cover whole: the colour by CopySubresourceRegion, the depth by a small pass of our own (a depth texture cannot be
// copied in part), in a device context state of its own (the game's state untouched; r2motion.c does the same).
typedef HRESULT(WINAPI *ssr_comp_fn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, void *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);
static ID3D11DeviceContext1 *g_ssr_ctx1;
static ID3DDeviceContextState *g_ssr_state;
static ID3D11VertexShader *g_ssr_vs;
static ID3D11PixelShader *g_ssr_ps;
static ID3D11Texture2D *g_crop[2];             // the corner of the scene copy, of the depth (R32_FLOAT)
static ID3D11ShaderResourceView *g_crop_srv[2];
static ID3D11RenderTargetView *g_crop_rtv;     // the depth corner's
static ID3D11ShaderResourceView *g_ssr_game[2];   // the game's views, put back after the draw
static int g_ssr_on, g_ssr_bad;
static unsigned g_ssr_draws;
static int ssr_init(void) {
    static const char src[] =
        "Texture2D<float> d : register(t0);\n"
        "float4 vs(uint i : SV_VertexID) : SV_Position { float2 t = float2((i << 1) & 2, i & 2); return float4(t * float2(2, -2) + float2(-1, 1), 0, 1); }\n"
        "float ps(float4 p : SV_Position) : SV_Target { return d.Load(int3(p.xy, 0)); }\n";
    static const GUID iid_dev1 = {0xa04bfb29, 0x08ef, 0x43d6, {0xa4, 0x9c, 0xa9, 0xbd, 0xbd, 0xcb, 0xe6, 0x86}};
    static const GUID iid_ctx1 = {0xbb2c6faa, 0xb5fb, 0x4082, {0x8e, 0x6b, 0x38, 0x8b, 0x8c, 0xfa, 0x90, 0xe1}};
    static const GUID iid_dev = {0xdb6f6ddb, 0xac77, 0x4e88, {0x82, 0x53, 0x81, 0x9d, 0xf9, 0xbb, 0xf1, 0x40}};
    ID3D11Device1 *dev1 = NULL;
    ID3DBlob *v = NULL, *p = NULL;
    D3D_FEATURE_LEVEL fl = ID3D11Device_GetFeatureLevel(g_dev);
    UINT sflags = ID3D11Device_GetCreationFlags(g_dev) & D3D11_CREATE_DEVICE_SINGLETHREADED ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
    ssr_comp_fn comp = (ssr_comp_fn)(void *)GetProcAddress(LoadLibraryExA("d3dcompiler_47.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32), "D3DCompile");
    int ok = SUCCEEDED(ID3D11Device_QueryInterface(g_dev, &iid_dev1, (void **)&dev1)) &&
             SUCCEEDED(ID3D11DeviceContext_QueryInterface(g_ctx, &iid_ctx1, (void **)&g_ssr_ctx1)) &&
             SUCCEEDED(ID3D11Device1_CreateDeviceContextState(dev1, sflags, &fl, 1, D3D11_SDK_VERSION, &iid_dev, NULL, &g_ssr_state)) &&
             comp && SUCCEEDED(comp(src, sizeof src - 1, "ssrcrop", NULL, NULL, "vs", "vs_5_0", 0, 0, &v, NULL)) &&
             SUCCEEDED(comp(src, sizeof src - 1, "ssrcrop", NULL, NULL, "ps", "ps_5_0", 0, 0, &p, NULL)) &&
             SUCCEEDED(ID3D11Device_CreateVertexShader(g_dev, ID3D10Blob_GetBufferPointer(v), ID3D10Blob_GetBufferSize(v), NULL, &g_ssr_vs)) &&
             SUCCEEDED(ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(p), ID3D10Blob_GetBufferSize(p), NULL, &g_ssr_ps));
    if (dev1) ID3D11Device1_Release(dev1);
    if (v) ID3D10Blob_Release(v);
    if (p) ID3D10Blob_Release(p);
    if (!ok) g_log("DLSS: water reflection fix not available (no D3D11.1 state or shaders) - reflections of ships stay off below native size");
    return ok;
}
static ID3D11Texture2D *ssr_tex(int i, DXGI_FORMAT res, DXGI_FORMAT view, UINT w, UINT h) {   // the crop i, (re)made at w x h
    D3D11_TEXTURE2D_DESC d;
    if (g_crop[i]) {
        ID3D11Texture2D_GetDesc(g_crop[i], &d);
        if (d.Width == w && d.Height == h && d.Format == res) return g_crop[i];
        ID3D11ShaderResourceView_Release(g_crop_srv[i]);
        if (i) ID3D11RenderTargetView_Release(g_crop_rtv);
        ID3D11Texture2D_Release(g_crop[i]);
        g_crop[i] = NULL;
    }
    D3D11_TEXTURE2D_DESC nd = {w, h, 1, 1, res, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | (i ? D3D11_BIND_RENDER_TARGET : 0), 0, 0};
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {view, D3D11_SRV_DIMENSION_TEXTURE2D};
    sd.Texture2D.MipLevels = 1;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &nd, NULL, &g_crop[i]))) { g_crop[i] = NULL; return NULL; }
    if (FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)g_crop[i], &sd, &g_crop_srv[i])) ||
        (i && FAILED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)g_crop[i], NULL, &g_crop_rtv)))) {
        ID3D11Texture2D_Release(g_crop[i]);   // shortcut: a view made before the failure stays (never seen to fail)
        g_crop[i] = NULL;
    }
    return g_crop[i];
}
// the top-left w x h of a depth view into g_crop[1] (R32_FLOAT), by a pass in a device context state of our own
static int crop_depth(ID3D11DeviceContext *c, ID3D11ShaderResourceView *src, UINT w, UINT h) {
    if (g_ssr_bad || (!g_ssr_state && !ssr_init())) { g_ssr_bad = 1; return 0; }
    if (!ssr_tex(1, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, w, h)) return 0;
    r2m_internal = 1;
    ID3DDeviceContextState *game = NULL;
    ID3D11DeviceContext1_SwapDeviceContextState(g_ssr_ctx1, g_ssr_state, &game);
    D3D11_VIEWPORT vp = {0, 0, (float)w, (float)h, 0, 1};
    ID3D11DeviceContext_OMSetRenderTargets(c, 1, &g_crop_rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(c, 1, &vp);
    ID3D11DeviceContext_IASetInputLayout(c, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(c, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(c, g_ssr_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(c, g_ssr_ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(c, 0, 1, &src);
    ID3D11DeviceContext_Draw(c, 3, 0);
    ID3D11ShaderResourceView *none = NULL;
    ID3D11DeviceContext_PSSetShaderResources(c, 0, 1, &none);
    ID3D11DeviceContext_OMSetRenderTargets(c, 0, NULL, NULL);
    ID3D11DeviceContext1_SwapDeviceContextState(g_ssr_ctx1, game, NULL);
    if (game) ID3DDeviceContextState_Release(game);
    r2m_internal = 0;
    hook_context();   // the swap puts d3d11's own entries back (bridgelive_test: the next target change went unseen)
    return 1;
}
static void ssr_begin(ID3D11DeviceContext *c) {   // before each draw into the reflection
    if (g_ssr_bad || !g_jit_on || (g_rw == g_live_w && g_rh == g_live_h)) return;
    ID3D11PixelShader *ps = NULL;
    ID3D11DeviceContext_PSGetShader(c, &ps, NULL, NULL);
    int ssr = 0;
    for (LONG i = g_novps < 64 ? g_novps : 64; ps && i-- > 0;) if (g_ovps[i].ps == ps) ssr = g_ovps[i].ssr;
    if (ps) ID3D11PixelShader_Release(ps);
    if (!ssr) return;
    ID3D11DeviceContext_PSGetShaderResources(c, 0, 2, g_ssr_game);
    ID3D11Resource *r[2] = {NULL, NULL};
    D3D11_TEXTURE2D_DESC d[2];
    D3D11_SHADER_RESOURCE_VIEW_DESC vd[2];
    int k[2] = {0, 0};
    for (int i = 0; i < 2 && g_ssr_game[i]; i++) {
        ID3D11ShaderResourceView_GetResource(g_ssr_game[i], &r[i]);
        ID3D11ShaderResourceView_GetDesc(g_ssr_game[i], &vd[i]);
        D3D11_RESOURCE_DIMENSION dim;
        ID3D11Resource_GetType(r[i], &dim);
        if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) ID3D11Texture2D_GetDesc((ID3D11Texture2D *)r[i], &d[i]), k[i] = screen_part(d[i].Width, d[i].Height);
    }
    // the corner: the viewport shrink's size (render size / k), at least the scene copy's and the depth's
    ID3D11Texture2D *t0 = k[0] ? ssr_tex(0, d[0].Format, vd[0].Format, (g_rw + k[0] - 1) / k[0], (g_rh + k[0] - 1) / k[0]) : NULL;
    if (t0 && k[1] && d[1].SampleDesc.Count == 1 && crop_depth(c, g_ssr_game[1], (g_rw + k[1] - 1) / k[1], (g_rh + k[1] - 1) / k[1])) {
        D3D11_BOX box = {0, 0, 0, (g_rw + k[0] - 1) / k[0], (g_rh + k[0] - 1) / k[0], 1};
        ID3D11DeviceContext_CopySubresourceRegion(c, (ID3D11Resource *)t0, 0, 0, 0, 0, r[0], 0, &box);
        ID3D11DeviceContext_PSSetShaderResources(c, 0, 2, g_crop_srv);
        if (!g_ssr_draws++) g_log("DLSS: water reflection's screen-space pass reads the render-size corner of the scene (%ux%u) and its depth", box.right, box.bottom);
        g_ssr_on = 1;
    }
    for (int i = 0; i < 2; i++) if (r[i]) ID3D11Resource_Release(r[i]);
    if (!g_ssr_on)
        for (int i = 0; i < 2; i++) if (g_ssr_game[i]) ID3D11ShaderResourceView_Release(g_ssr_game[i]), g_ssr_game[i] = NULL;
}
static void ssr_end(ID3D11DeviceContext *c) {   // after it: the game's views back
    if (!g_ssr_on) return;
    g_ssr_on = 0;
    hook_context();   // d3d11 put its entries back once more at the first call after the swap (the draw)
    ID3D11DeviceContext_PSSetShaderResources(c, 0, 2, g_ssr_game);
    for (int i = 0; i < 2; i++) if (g_ssr_game[i]) ID3D11ShaderResourceView_Release(g_ssr_game[i]), g_ssr_game[i] = NULL;
}

// Below native size, at the draws of a pixel shader with its camera block outside slot 0 (g_camps): the patched copy in
// that slot for the draw. The terrain's virtual-texture page touch (virtualtexture.hlsl ps30_page_touch, into a quarter-
// size target the CPU scans for the texture pages to load, tilescan.c) instead runs as at native size: uv = (SV_Position
// + offset) * g_page_touch_constants.xy over the whole target, depth there, world position by the camera's inverse
// view-projection, then the page and its level from that position's screen derivatives. Shrunk with the scene it got the
// game's camera (cb1, not patched: the requests covered the top-left 2/3 of the view) and derivatives 1.5x too large at
// Quality (one texture level coarser); drawn over the whole target from a render-size crop of the depth, its own uv and
// the game's camera see the full view at native density.
static ID3D11Buffer *g_cs_game;
static int g_cs_slot = -1, g_pt_on;
static ID3D11ShaderResourceView *g_pt_game;
static D3D11_VIEWPORT g_pt_vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
static UINT g_pt_nvp;
static unsigned g_cs_draws, g_pt_draws;
static void pt_begin(ID3D11DeviceContext *c) {
    ID3D11ShaderResourceView *v = NULL;
    ID3D11DeviceContext_PSGetShaderResources(c, 0, 1, &v);
    if (!v) return;
    ID3D11Resource *r = NULL;
    D3D11_RESOURCE_DIMENSION dim;
    D3D11_TEXTURE2D_DESC d = {0};
    ID3D11ShaderResourceView_GetResource(v, &r);
    ID3D11Resource_GetType(r, &dim);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) ID3D11Texture2D_GetDesc((ID3D11Texture2D *)r, &d);
    ID3D11Resource_Release(r);
    if (d.Width != g_live_w || d.Height != g_live_h || d.SampleDesc.Count != 1 || !crop_depth(c, v, g_rw, g_rh)) { ID3D11ShaderResourceView_Release(v); return; }
    g_pt_game = v;
    ID3D11DeviceContext_PSSetShaderResources(c, 0, 1, &g_crop_srv[1]);
    D3D11_VIEWPORT full = {0, 0, (float)g_rt_w, (float)g_rt_h, 0, 1};
    g_pt_nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    r2m_internal = 1;
    ID3D11DeviceContext_RSGetViewports(c, &g_pt_nvp, g_pt_vp);
    ID3D11DeviceContext_RSSetViewports(c, 1, &full);
    r2m_internal = 0;
    g_pt_on = 1;
    if (!g_pt_draws++) g_log("DLSS: the terrain's texture page requests are made over the whole view at native density (%ux%u from %ux%u)",
                             g_rt_w, g_rt_h, g_rw, g_rh);
}
static void cs_begin(ID3D11DeviceContext *c) {
    int scaled = g_rw != g_live_w || g_rh != g_live_h;
    if (!g_jit_on || g_hud_frame == g_frame || (!scaled && (!(g_jx || g_jy) || g_jit_frame != g_frame)) || g_rt_overlay == 1 || !g_rt_w ||
        !screen_part(g_rt_w, g_rt_h)) return;
    ID3D11PixelShader *ps = NULL;
    ID3D11DeviceContext_PSGetShader(c, &ps, NULL, NULL);
    if (ps) ID3D11PixelShader_Release(ps);
    unsigned slot;
    int pt;
    if (!ps || !camps_find(ps, &slot, &pt) || slot >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT) return;
    if (pt) { if (scaled) pt_begin(c); return; }
    ID3D11Buffer *b = NULL;
    ID3D11DeviceContext_PSGetConstantBuffers(c, slot, 1, &b);
    CamCopy *e = b ? cam_copy((ID3D11Resource *)b, 0) : NULL;
    ID3D11Buffer *want = !e ? NULL : g_rt_overlay == 2 ? (scaled && e->bl_valid ? e->bl : NULL) : e->valid ? e->ours : NULL;
    if (!want) { if (b) ID3D11Buffer_Release(b); return; }
    r2m_internal = 1;
    ID3D11DeviceContext_PSSetConstantBuffers(c, slot, 1, &want);
    r2m_internal = 0;
    g_cs_game = b;
    g_cs_slot = (int)slot;
    if (!g_cs_draws++) g_log("DLSS: a pixel shader with its camera block in slot %u gets the patched copy below native size", slot);
}
static void cs_end(ID3D11DeviceContext *c) {   // after that draw: the game's camera, views and viewports back
    if (g_pt_on) {
        g_pt_on = 0;
        hook_context();   // d3d11 put its entries back once more at the first call after the depth pass's swap (the draw)
        r2m_internal = 1;
        ID3D11DeviceContext_RSSetViewports(c, g_pt_nvp, g_pt_vp);
        r2m_internal = 0;
        ID3D11DeviceContext_PSSetShaderResources(c, 0, 1, &g_pt_game);
        ID3D11ShaderResourceView_Release(g_pt_game);
        g_pt_game = NULL;
    }
    if (g_cs_slot < 0) return;
    r2m_internal = 1;
    ID3D11DeviceContext_PSSetConstantBuffers(c, (UINT)g_cs_slot, 1, &g_cs_game);
    r2m_internal = 0;
    if (g_cs_game) ID3D11Buffer_Release(g_cs_game);
    g_cs_game = NULL;
    g_cs_slot = -1;
}

// the F11 panel's switches (from the next frame; not saved: the next start takes fgbridge_on.txt again)
// render size per mode: off and DLAA native, then NVIDIA's preset scales (Quality 2/3, Balanced 0.58, Performance 1/2,
// Ultra Performance 1/3), rounded down to even
static const char *const SR_NAMES[6] = {"off", "DLAA", "Quality", "Balanced", "Performance", "Ultra Performance"};
static void sr_set(int mode) {
    static const float scale[6] = {1, 1, 2.0f / 3, 0.58f, 0.5f, 1.0f / 3};
    g_sr = mode >= 0 && mode < 6 ? mode : 0;
    g_rw = (UINT)(g_live_w * scale[g_sr]) & ~1u;
    g_rh = (UINT)(g_live_h * scale[g_sr]) & ~1u;
    fgb_set_sr(g_sr != 0, g_rw, g_rh);
}
// choices are saved for the next start: the release add-on's FrameBridge\settings.txt (one key=value per line, as Attila's),
// in dev runs fgbridge_on.txt (lines 3 and 4)
static const char *const SR_KEYS[6] = {"off", "dlaa", "quality", "balanced", "performance", "ultra_performance"};
static const char *const FG_KEYS[4] = {"off", "2x", "3x", "4x"};
// the frame cap: off, auto (default), or a fixed rendered-frame cap
static const char *const CAP_KEYS[5] = {"off", "auto", "60", "90", "120"};
static const unsigned CAP_FPS[5] = {0, 0, 60, 90, 120};
static int g_fcap = 1;   // the frame cap choice
// the DLSS preset (NVIDIA's model): default (NVIDIA's pick per mode: K for DLAA / Quality / Balanced, M Performance,
// L Ultra Performance) or one for every mode; handed to the helper as NVIDIA's preset number
static const char *const PRESET_KEYS[5] = {"default", "j", "k", "l", "m"};
static const unsigned PRESET_NGX[5] = {0, 10, 11, 12, 13};
static int g_preset;
// sharpening of the DLSS output (fgbridge.c: AMD's RCAS): off, or RCAS's con at 1.25 / 0.6 / 0.2 stops (AMD's default 0.2)
static const char *const SHARP_KEYS[4] = {"off", "low", "medium", "high"};
static const float SHARP_CON[4] = {0, 0.42f, 0.66f, 0.87f};
static int g_sharp = 1;
static void settings_save(void) {
    if (g_settings[0]) {
        FILE *f = _wfopen(g_settings, L"w");
        if (!f) { g_log("DLSS settings: %ls not writable - choices not saved", g_settings); return; }
        fprintf(f, "# DLSS add-on settings. The F11 panel in game changes and saves them.\n"
                   "# super_resolution: off, dlaa, quality, balanced, performance, ultra_performance\n"
                   "# frame_generation: off, 2x, 3x, 4x (limited to what your GPU allows)\n"
                   "# motion_vectors: camera_only, per_object (cleaner frame generation on moving soldiers and horses)\n"
                   "# frame_cap: off, auto, 60, 90, 120 (rendered frames; auto: the screen's refresh rate / the frames shown per\n"
                   "#            rendered frame with frame generation on, e.g. 120 at 2x on a 240 Hz screen)\n"
                   "# dlss_preset: default, j, k, l, m (NVIDIA's DLSS model; default: K for DLAA / Quality / Balanced, M for\n"
                   "#              Performance, L for Ultra Performance)\n"
                   "# sharpness: off, low, medium, high (sharpening of the DLSS output, as games with DLSS built in have)\n"
                   "# addon: on, off (off: the add-on does nothing at all from the next game start; set on again to use it)\n"
                   "super_resolution=%s\nframe_generation=%s\nmotion_vectors=%s\nframe_cap=%s\ndlss_preset=%s\nsharpness=%s\naddon=on\nversion=2\n",
                SR_KEYS[g_sr >= 0 && g_sr < 6 ? g_sr : 2], FG_KEYS[g_fg < 4 ? g_fg : 0], g_motion_on ? "per_object" : "camera_only",
                CAP_KEYS[g_fcap >= 0 && g_fcap < 5 ? g_fcap : 1], PRESET_KEYS[g_preset >= 0 && g_preset < 5 ? g_preset : 0],
                SHARP_KEYS[g_sharp >= 0 && g_sharp < 4 ? g_sharp : 1]);
        fclose(f);
        return;
    }
    // dev: line 4 = words (super resolution, "motion", "cap<key>" unless auto, "preset<key>" unless default, "sharp<key>" unless
    // low, the test options kept)
    static const char *const words[6] = {"", "dlaa", "quality", "balanced", "perf", "ultraperf"};
    char l4[128] = {0};
    const char *w[6] = {words[g_sr], g_motion_on ? "motion" : "", "", "", "", g_on_test};
    char capw[16] = {0}, prw[16] = {0}, shw[16] = {0};
    if (g_fcap != 1) { snprintf(capw, sizeof capw, "cap%s", CAP_KEYS[g_fcap]); w[2] = capw; }
    if (g_preset) { snprintf(prw, sizeof prw, "preset%s", PRESET_KEYS[g_preset]); w[3] = prw; }
    if (g_sharp != 1) { snprintf(shw, sizeof shw, "sharp%s", SHARP_KEYS[g_sharp]); w[4] = shw; }
    for (int i = 0; i < 6; i++)
        if (w[i][0]) snprintf(l4 + strlen(l4), sizeof l4 - strlen(l4), "%s%s", l4[0] ? " " : "", w[i]);
    wchar_t p[MAX_PATH];
    _snwprintf(p, MAX_PATH, L"%ls\\fgbridge_on.txt", g_dir);
    FILE *f = _wfopen(p, L"w");
    if (!f) { g_log("F11 panel: fgbridge_on.txt not writable - choices not saved"); return; }
    fprintf(f, "%s\n%s\n%u\n%s\n", g_on_lines[0], g_on_lines[1], g_fg, l4);
    fclose(f);
}
static int settings_value(const char *text, const char *key, const char *const *names, int n, int fallback) {
    const char *v = strstr(text, key);
    if (!v) return fallback;
    v += strlen(key);
    for (int i = n; i-- > 0;)   // a value counts up to the line end
        if (!strncmp(v, names[i], strlen(names[i])) && (v[strlen(names[i])] == 0 || strchr("\r\n \t", v[strlen(names[i])]))) return i;
    return fallback;
}
static void panel_sr(int mode) {
    sr_set(mode);
    g_log("F11 panel: super resolution %s, render %ux%u", SR_NAMES[g_sr], g_rw, g_rh);
    settings_save();
}
static void panel_sharp(int choice) {
    g_sharp = choice >= 0 && choice < 4 ? choice : 1;
    fgb_set_sharpness(SHARP_CON[g_sharp]);
    g_log("F11 panel: sharpness %s", SHARP_KEYS[g_sharp]);
    settings_save();
}
static void panel_preset(int choice) {
    g_preset = choice >= 0 && choice < 5 ? choice : 0;
    fgb_set_preset(PRESET_NGX[g_preset]);
    g_log("F11 panel: DLSS preset %s", PRESET_KEYS[g_preset]);
    settings_save();
}
static void panel_motion(int on) {
    g_motion_on = on;
    r2m_set_enabled(on);
    g_log("F11 panel: per-object motion %s", on ? "on" : "off");
    settings_save();
}
#ifdef PROFILING   // the debug view: dev builds only (release: g_view stays 0, no panel row)
static void panel_view(int v) {   // for this session only (a debug aid, not saved): 0 off, 1 no texture mip bias, 2 no jitter, 3 soldiers' motion
    g_view = v == 3;
    g_lod_off = v == 1;
    g_jit_off = v == 2;
    r2m_set_view(g_view);
    g_log("F11 panel: debug view %s", v == 3 ? "soldiers' motion" : v == 2 ? "no jitter" : v ? "no texture mip bias" : "off");
}
#define PANEL_VIEW panel_view
static void panel_jtest(int m) {   // for this session only (a debug aid, not saved)
    static const char *const names[6] = {"normal", "tell 0", "flip x", "flip y", "previous frame's", "next frame's"};
    g_jtest = m >= 0 && m < 6 ? m : 0;
    g_log("F11 panel: jitter test %s", names[g_jtest]);
}
#define PANEL_JTEST panel_jtest
#else
#define PANEL_VIEW NULL
#define PANEL_JTEST NULL
#endif
// fgbridge.c's motion callback (after its camera-motion pass, before the HUD-less inputs go out): the animated models
// again, with this frame's main camera, through the scene's viewport (render size, jitter offset)
static void motion_cb(ID3D11Resource *depth, float jx, float jy) {
    (void)depth;
    float cam[52] = {0};
    memcpy(cam + 36, g_vp, 64);
    r2m_pass(g_scene_dsv, cam, g_rw, g_rh, jx, jy, 0, 1);
}
// The frame cap on rendered frames, through NVIDIA Reflex's limiter in the helper. Auto (the default): with frame
// generation on, the screen's refresh rate / the frames shown per rendered frame (240 Hz, 2x: 120 rendered, 240 shown),
// so every rendered frame gets its generated frames instead of the helper dropping them past the screen's rate (a
// 162 fps game at 2x showed ~185 on a 240 Hz screen); no cap without frame generation. Reflex counts shown frames.
static unsigned g_refresh;   // the game window's screen, Hz (0 unknown: Auto does nothing)
static unsigned g_cap_fps;   // the rendered-frame cap in effect (0 none)
static unsigned refresh_hz(HWND w) {
    MONITORINFOEXW mi;
    DEVMODEW dm;
    memset(&mi, 0, sizeof mi); mi.cbSize = sizeof mi;
    memset(&dm, 0, sizeof dm); dm.dmSize = sizeof dm;
    HMONITOR m = MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST);
    if (!m || !GetMonitorInfoW(m, (MONITORINFO *)&mi) || !EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) return 0;
    return dm.dmDisplayFrequency > 1 ? dm.dmDisplayFrequency : 0;   // 0 / 1: "the hardware's default"
}
static void cap_apply(void) {
    unsigned per = g_fg + 1;
    g_cap_fps = g_fcap == 1 ? (g_fg && g_refresh ? g_refresh / per : 0) : CAP_FPS[g_fcap];
    // microseconds per shown frame, rounded up: never above the screen's rate (4167 us = 239.98 fps at 240 Hz)
    fgb_set_frame_limit(g_cap_fps ? (unsigned)ceil(1e6 / ((double)g_cap_fps * per)) : 0);
    fgp_set_cap(g_fcap, g_cap_fps);
    if (g_cap_fps) g_log("frame cap %s: %u rendered fps, %u shown (screen %u Hz, frame generation %ux)", CAP_KEYS[g_fcap], g_cap_fps,
                         g_cap_fps * per, g_refresh, per);
    else g_log("frame cap %s: none%s", CAP_KEYS[g_fcap], g_fcap == 1 ? (g_refresh ? " (frame generation off)" : " (screen rate unknown)") : "");
}
static void panel_cap(int choice) {
    g_fcap = choice >= 0 && choice < 5 ? choice : 1;
    cap_apply();
    settings_save();
}
static void panel_fg(int n) {
    g_fg = (unsigned)n;
    fgb_set_fg((unsigned)n);
    g_log("F11 panel: frame generation %s", n ? (n == 1 ? "2x" : n == 2 ? "3x" : "4x") : "off");
    cap_apply();   // Auto and the fixed caps depend on the frames shown per rendered frame
    settings_save();
}
// The F11 panel after the bridge (Attila 6.4): DLSS stays off until the game restarts, but F11 still opens the panel over
// the frames the mod shows, on that frame's device, with a note why. Choices made there are saved for the next start.
static const wchar_t *g_off_note = L"DLSS is off - restart the game to turn it back on. Choices made here are saved.";
static void fallback_panel(ID3D11Resource *bb) {
    static ID3D11Device *pdev;
    static int ok;
    ID3D11Device *d = NULL;
    ID3D11Resource_GetDevice(bb, &d);
    if (!d) return;
    if (d != pdev) {
        ID3D11DeviceContext *c = NULL;
        D3D11_TEXTURE2D_DESC td;
        ID3D11Device_GetImmediateContext(d, &c);
        ID3D11Texture2D_GetDesc((ID3D11Texture2D *)bb, &td);
        ok = c && fgp_move_device(d, c, td.Width, td.Height);   // the panel keeps these references
        fgp_set_note(g_off_note);
        if (pdev) ID3D11Device_Release(pdev);
        pdev = d;   // GetDevice's reference
        g_log("F11 panel: %s over the frames the mod shows (DLSS off until the game restarts)", ok ? "available" : "NOT available");
    } else
        ID3D11Device_Release(d);
    if (ok) fgp_frame(bb, g_out_hwnd);
}
static void panel_frame(ID3D11Resource *bb) {   // at Present, before the frame goes to the helper: the panel is UI
    static LARGE_INTEGER t0, qf;
    static unsigned frames, shown0;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    if (!qf.QuadPart) { QueryPerformanceFrequency(&qf); t0 = t; shown0 = fgb_shown(); }
    frames++;
    double dt = (double)(t.QuadPart - t0.QuadPart) / qf.QuadPart;
    if (dt >= 0.5) {   // the game's frame rate and the shown one (generated frames included)
        unsigned sh = fgb_shown();
        fgp_set_fps(frames / dt, (sh - shown0) / dt);
        frames = 0; t0 = t; shown0 = sh;
    }
    fgp_frame(bb, g_out_hwnd);
}

static void patch_slot(void **slot, void *to) {
    DWORD prot;
    if (!VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &prot)) return;
    *slot = to;
    VirtualProtect(slot, sizeof *slot, prot, &prot);
}
#define SLOT(name) (offsetof(ID3D11DeviceContextVtbl, name) / sizeof(void *))
static void hook_context(void) {
    if (!g_ctx) return;
    static const unsigned idx[NSLOT] = {SLOT(OMSetRenderTargets), SLOT(OMSetRenderTargetsAndUnorderedAccessViews), SLOT(VSSetConstantBuffers),
                                        SLOT(ClearRenderTargetView), SLOT(ClearDepthStencilView), SLOT(Map), SLOT(Unmap),
                                        SLOT(UpdateSubresource), SLOT(DrawIndexed), SLOT(Draw), SLOT(DrawIndexedInstanced), SLOT(DrawInstanced),
                                        SLOT(RSSetViewports), SLOT(RSGetViewports), SLOT(OMSetDepthStencilState), SLOT(PSSetConstantBuffers), SLOT(VSSetShader),
                                        SLOT(PSSetSamplers), SLOT(Dispatch), SLOT(PSSetShader)};
    void *const fn[NSLOT] = {(void *)w_omrt, (void *)w_omrtuav, (void *)w_vscb, (void *)w_crtv, (void *)w_cdsv, (void *)w_map,
                             (void *)w_unmap, (void *)w_upd, (void *)w_dip, (void *)w_d, (void *)w_dii, (void *)w_di,
                             (void *)w_rsvp, (void *)w_rsgetvp, (void *)w_omdss, (void *)w_pscb, (void *)w_vsset, (void *)w_pssamp, (void *)w_disp, (void *)w_psset};
    // This runs at every target change and constant-buffer bind (thousands per frame in battle), so no system calls on
    // the way: d3d11's address range is read once, and a table in writable memory (the context's table is a heap copy
    // that d3d11 itself rewrites) is written directly. (Run 2: the game crawled zoomed in on moving units; guess: the
    // GetModuleHandleEx + VirtualProtect calls made for every slot d3d11 had swapped back.)
    static UINT_PTR d3d_lo, d3d_hi;
    static int foreign;
    if (!d3d_lo) {
        const unsigned char *m = (const unsigned char *)GetModuleHandleW(L"d3d11.dll");
        if (!m) return;
        d3d_lo = (UINT_PTR)m;
        d3d_hi = d3d_lo + ((const IMAGE_NT_HEADERS *)(m + ((const IMAGE_DOS_HEADER *)m)->e_lfanew))->OptionalHeader.SizeOfImage;
    }
    g_rh_calls++;
    void **vt = *(void ***)g_ctx;
    int i = 0;
    while (i < g_nvt && g_vts[i].vt != vt) i++;
    if (i == g_nvt) {
        if (g_nvt == 16) return;   // shortcut: 16 tables at most (2 seen offline)
        g_vts[g_nvt].vt = vt;
        g_vts[g_nvt].rw = 1;
        for (int s = 0; s < NSLOT; s += NSLOT - 1) {   // the first and the last slot: the table may cross a page
            MEMORY_BASIC_INFORMATION mb;
            g_vts[g_nvt].rw &= VirtualQuery(&vt[idx[s]], &mb, sizeof mb) && mb.State == MEM_COMMIT &&
                               (mb.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
        }
        g_nvt++;
    }
    LARGE_INTEGER t0 = {0};
    for (int s = 0; s < NSLOT; s++) {
        void *now = vt[idx[s]];
        if (now == fn[s]) continue;
        if (!t0.QuadPart) QueryPerformanceCounter(&t0);
        if (g_vts[i].orig[s] && ((UINT_PTR)now < d3d_lo || (UINT_PTR)now >= d3d_hi)) {
            // not d3d11 swapping its own functions back: another mod hooked the slot after us and calls ours as its
            // original; hooking again would make ours call it back forever (Attila 6.3 stack overflow). Left to it.
            if (!foreign++) {
                HMODULE owner = NULL;
                wchar_t name[MAX_PATH] = L"?";
                GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)now, &owner);
                if (owner) GetModuleFileNameW(owner, name, MAX_PATH);
                const wchar_t *b = wcsrchr(name, L'\\');
                g_log("bridge probe: context slot %u is now hooked by %ls - left to it (only logged once)", idx[s], b ? b + 1 : name);
            }
            g_rh_foreign++;
            continue;
        }
        g_vts[i].orig[s] = now;
        if (g_vts[i].rw) vt[idx[s]] = fn[s];
        else patch_slot(&vt[idx[s]], fn[s]);
        g_rh_patches++;
    }
    if (t0.QuadPart) {
        LARGE_INTEGER t1;
        QueryPerformanceCounter(&t1);
        g_rh_ticks += t1.QuadPart - t0.QuadPart;
    }
}

static void bp_report(void) {
    g_log("bridge probe: %u presents (%u not on the first Present's thread), sync interval 0/1/2/3/more: %u/%u/%u/%u/%u, captures %d, "
          "camera writes recorded %u%s", g_presents, g_present_other_thread, g_sync[0], g_sync[1], g_sync[2], g_sync[3], g_sync[4],
          g_captures, g_cam_writes, g_sc ? "" : " - no swapchain seen");
    g_log("  D3D11CreateDevice calls seen %d, device %s, nested Presents sent to DXGI %u", g_creates, g_ctx ? "known" : "not known yet", g_nested);
    if (g_live)
        g_log("  frame bridge %s: %u frames handed over, %u pacing timeouts, HUD-less copies %u, no camera %u, MSAA depth seen %u, "
              "fallback frames %u", g_live_state == 1 ? "running" : g_live_state == 2 ? "off" : "not started", fgb_frames(),
              fgb_pace_timeouts(), g_br_hud, g_br_novp, g_br_msaa, g_fb_frames);
    if (g_live_state == 1 && g_sr) {   // this period only
        unsigned sf, st;
        double sa, sm;
        fgb_sr_stats(&sf, &st, &sa, &sm);
        g_log("  DLAA: %u frames jittered, %u viewports jittered; DLSS round trips %u, game CPU wait avg %.3f ms max %.3f ms, %u timeouts",
              g_jit_frames, g_jit_vp, sf, sa, sm, st);
        g_log("  super resolution %s, render %ux%u: native frames %u, camera copies for pixel shaders written %u", SR_NAMES[g_sr], g_rw,
              g_rh, g_br_native, g_ccopy_writes);
        unsigned nb = 0;
        for (unsigned i = 0; i < g_nlod; i++) nb += g_lod[i].ours != NULL;
        g_log("  texture mip bias %.2f: %u of %u game samplers biased, %u sampler binds swapped%s", g_lod_bias, nb, g_nlod, g_lod_swaps,
              g_lod_full ? " (sampler table full)" : "");
        g_ccopy_writes = 0;
        g_lod_swaps = 0;
        g_jit_frames = g_jit_vp = 0;
    }
    if (g_live_state == 1) {   // this period only
        double hm, pm, wm;
        unsigned fr;
        fgb_cpu_stats(&hm, &pm, &wm, &fr);
        if (g_motion) r2m_report(g_log);
        if (g_fl_frames)
            g_log("  motion view: %u frames, %u with a camera write other than the one taken, largest camera change %.4f, flashes %u", g_fl_frames,
                  g_fl_multi, g_fl_max_change, g_fl_flashes);
        g_fl_frames = g_fl_multi = 0;
        g_fl_max_change = 0;
        g_log("  last 30 s: frames with the scene depth %u, camera writes %u (%u with the scene depth bound); main-thread time per "
              "frame: HUD-less step %.3f ms, Present step %.3f ms, of which waiting for the helper %.3f ms", g_br_scene_frames, g_br_camw,
              g_br_camw_scene, fr ? hm / fr : 0, fr ? pm / fr : 0, fr ? wm / fr : 0);
        g_br_scene_frames = g_br_camw = g_br_camw_scene = 0;
        LARGE_INTEGER q;
        QueryPerformanceFrequency(&q);
        double ms = 1000.0 / q.QuadPart, n = g_n_frames ? g_n_frames : 1;
        g_log("  last 30 s, the game's D3D11 calls: Map %.0f per frame, %.3f ms per frame (longest %.2f ms); UpdateSubresource "
              "%.0f per frame, %.3f ms per frame (longest %.2f ms); longest frame %.1f ms", g_n_map / n, g_t_map * ms / n,
              g_t_map_max * ms, g_n_upd / n, g_t_upd * ms / n, g_t_upd_max * ms, g_t_frame_max * ms);
        g_t_map = g_t_map_max = g_t_upd = g_t_upd_max = g_t_frame_max = 0;
        g_n_map = g_n_upd = g_n_frames = 0;
        double gf = g_glue_frames ? g_glue_frames : 1;
        static double empty;   // an empty measured span (the timer's own cost): taken off, 2 spans a draw, 1 a sampler bind
        if (!empty) {
            LARGE_INTEGER a, b;
            LONGLONG sum = 0;
            for (int i = 0; i < 20000; i++) { QueryPerformanceCounter(&a); QueryPerformanceCounter(&b); sum += b.QuadPart - a.QuadPart; }
            empty = sum / 20000.0;
        }
        double td = g_glue_t - 2 * empty * g_glue_draws, ts = g_glue_ts - empty * g_glue_sbinds;
        g_log("  last 30 s, the glue's own work: %.3f ms per frame at the game's draws (%.0f draws), %.3f ms at sampler binds (every 8th "
              "frame measured, the timer's own %.0f ns a span taken off)", (td > 0 ? td : 0) * ms / gf, g_glue_draws / gf,
              (ts > 0 ? ts : 0) * ms / gf, empty * ms * 1e6);
        g_glue_t = g_glue_ts = 0;
        g_glue_frames = g_glue_draws = g_glue_sbinds = 0;
    }
    LARGE_INTEGER qf;
    QueryPerformanceFrequency(&qf);
    g_log("  context re-hook checks %u, slots hooked again %u, foreign slots left %u, %.1f ms spent hooking again (%u tables, "
          "%s)", g_rh_calls, g_rh_patches, g_rh_foreign, g_rh_ticks * 1000.0 / qf.QuadPart, g_nvt,
          g_nvt && g_vts[0].rw ? "written directly" : "through VirtualProtect");
    g_rh_calls = g_rh_patches = g_rh_foreign = 0;
    g_rh_ticks = 0;
    for (int i = 0; i < 8 && g_callers[i].n; i++) {
        HMODULE mod = NULL;
        char name[MAX_PATH] = "?";
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(UINT_PTR)g_callers[i].ret, &mod);
        GetModuleFileNameA(mod, name, MAX_PATH);
        const char *b = strrchr(name, '\\');
        g_log("  Present called from %s+0x%X, first game return address on the stack +0x%X: %u times", b ? b + 1 : name,
              g_callers[i].ret - (unsigned)(UINT_PTR)mod, g_callers[i].game_ret, g_callers[i].n);
    }
}

// once per Present: ends a capture, arms / starts the next one
static void bp_frame(void) {
    g_frame++;
    g_glue_meas = (g_frame & 7) == 0;
    g_glue_frames += g_glue_meas;
    if (g_capturing) {
        g_capturing = 0;
        fprintf(g_cap, "== end of captured frame: %u draws, %u target changes\n", g_draws, g_sets);
        fclose(g_cap);
        g_cap = NULL;
    }
    if (g_camera_left && !--g_camera_left && g_cam) { fclose(g_cam); g_cam = NULL; }
    mp_frame();
    hook_context();   // cheap when the slots are still ours
    if (g_armed && g_sc && !g_camera_left) {   // a capture uses the swapchain's device (other devices may appear later)
        ID3D11Device *d = NULL;
        if (SUCCEEDED(IDXGISwapChain_GetDevice(g_sc, &iid_device, (void **)&d))) {
            if (d != g_dev) {
                if (g_ctx) ID3D11DeviceContext_Release(g_ctx);
                if (g_dev) ID3D11Device_Release(g_dev);
                if (g_staging) ID3D11Texture2D_Release(g_staging);
                g_staging = NULL;
                g_dev = d;
                ID3D11Device_GetImmediateContext(g_dev, &g_ctx);
                hook_context();
                g_log("bridge probe: capture uses the swapchain's device (not the first D3D11CreateDevice one)");
            } else ID3D11Device_Release(d);
        }
        g_armed = 0;
        g_captures++;
        wchar_t p[MAX_PATH];
        _snwprintf(p, MAX_PATH, L"%ls\\bridge_capture_%d.txt", g_dir, g_captures);
        g_cap = _wfopen(p, L"w");
        _snwprintf(p, MAX_PATH, L"%ls\\bridge_camera_%d.bin", g_dir, g_captures);
        g_cam = _wfopen(p, L"wb");
        if (g_cap) {
            DXGI_SWAP_CHAIN_DESC d;
            IDXGISwapChain_GetDesc(g_sc, &d);
            g_capturing = 1;
            g_dumps = g_sets = g_draws = 0;
            g_camera_left = 31;
            char t[600];
            set_begin(targets(t, sizeof t));
            fprintf(g_cap, "== bridge capture %d, frame %u; swapchain %ux%u format %u buffers %u swap effect %u windowed %d flags 0x%x\n"
                           "== targets at frame start: %s\n", g_captures, g_frame, d.BufferDesc.Width, d.BufferDesc.Height,
                    d.BufferDesc.Format, d.BufferCount, d.SwapEffect, d.Windowed, d.Flags, t);
            g_log("bridge probe: capture %d started (frame %u)", g_captures, g_frame);
        }
    }
    if ((g_frame & 31) == 0) {   // bridge_go.txt (made when the user says "ready") arms the next capture
        wchar_t p[MAX_PATH];
        _snwprintf(p, MAX_PATH, L"%ls\\bridge_go.txt", g_dir);
        if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES && DeleteFileW(p)) g_armed = 1;
    }
    static DWORD last;
    DWORD now = GetTickCount();
    if (!last) last = now;
    if (now - last >= 30000) { last = now; bp_report(); }
}

static HRESULT(STDMETHODCALLTYPE *r_present)(IDXGISwapChain *, UINT, UINT);
static HRESULT(STDMETHODCALLTYPE *r_create_sc)(IDXGIFactory *, IUnknown *, DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **);
// DXGI's own Present / CreateSwapChain (Attila 6.3.2): r_present / r_create_sc are what the slots held when hooked, which
// can be another mod's hook (the Steam overlay hooks early); a mod that hooks again on top of ours then leads back to us.
// The mod's own calls (fallback swapchain, nested Presents) go to DXGI's own, past any jump over its first bytes.
static HRESULT(STDMETHODCALLTYPE *g_present_real)(IDXGISwapChain *, UINT, UINT);
static HRESULT(STDMETHODCALLTYPE *g_create_real)(IDXGIFactory *, IUnknown *, DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **);
static void *dxgi_own(void **vt, size_t slot, void *saved, const char *name) {
    HMODULE m = NULL, sm = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)vt, &m);
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)saved, &sm);
    if (m && sm == m) return saved;
    void *orig = fgb_vt_original(vt, slot);
    char who[MAX_PATH] = "?";
    if (sm) GetModuleFileNameA(sm, who, MAX_PATH);
    const char *bn = strrchr(who, '\\');
    g_log("frame bridge: %s was already hooked by %s - the mod's own calls go to %s", name, bn ? bn + 1 : who,
          orig ? "DXGI's own (read from the DLL file)" : "that hook (DXGI's own not found)");
    return orig ? orig : saved;
}
static void *unhooked(void *fn, void **cache, const char *name) {
    if (*cache) return *cache;
    void *u = fgb_unhooked(fn);
    if (u && u != fn) g_log("frame bridge: DXGI's %s starts with a jump (another mod hooked it there) - the mod's own calls go past it", name);
    if (!u) g_log("frame bridge: DXGI's %s starts with a jump the mod cannot go past - it calls the hooked function", name);
    return *cache = u ? u : fn;
}
static HRESULT STDMETHODCALLTYPE dxgi_present(IDXGISwapChain *sc, UINT sync, UINT flags) {
    static void *cache;
    if (!g_present_real) return S_OK;
    return ((HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *, UINT, UINT))unhooked((void *)g_present_real, &cache, "Present"))(sc, sync, flags);
}
static HRESULT STDMETHODCALLTYPE dxgi_create_sc(IDXGIFactory *f, IUnknown *dev, DXGI_SWAP_CHAIN_DESC *d, IDXGISwapChain **out) {
    static void *cache;
    return ((HRESULT(STDMETHODCALLTYPE *)(IDXGIFactory *, IUnknown *, DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **))unhooked(
        (void *)g_create_real, &cache, "CreateSwapChain"))(f, dev, d, out);
}

// the bridge off for the session (helper gone, back buffer resized, new swapchain): the game keeps presenting on the
// hidden window and its frames reach the window through the mod's own swapchain (fgb_fallback_frame)
static void bridge_off(void) {
    fgb_stop();
    g_live_state = 2;
    if (g_motion) { r2m_set_enabled(0); r2m_frame_end(); g_motion = 0; }   // its draws hold the game's buffers
}

static HRESULT STDMETHODCALLTYPE present_body(IDXGISwapChain *sc, UINT sync, UINT flags);
// A Present inside a Present on the same thread (Attila 6.3.2: the Steam overlay's hook led through another mod back into
// ours, round and round until the stack overflowed) goes straight to DXGI. Logged once with its caller.
static HRESULT STDMETHODCALLTYPE w_present(IDXGISwapChain *sc, UINT sync, UINT flags) {
    static __thread int depth;
    if (depth) {
        if (!g_nested++) {
            unsigned ret = (unsigned)(UINT_PTR)__builtin_return_address(0);
            HMODULE m = NULL;
            char at[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(UINT_PTR)ret, &m))
                GetModuleFileNameA(m, at, MAX_PATH);
            const char *b = strrchr(at, '\\');
            g_log("bridge probe: Present called while a Present was running (from %s+0x%X) - it goes straight to DXGI (only logged once)",
                  b ? b + 1 : at, (unsigned)(ret - (unsigned)(UINT_PTR)m));
        }
        return dxgi_present(sc, sync, flags);
    }
    depth = 1;
    HRESULT hr = present_body(sc, sync, flags);
    depth = 0;
    return hr;
}
static HRESULT STDMETHODCALLTYPE present_body(IDXGISwapChain *sc, UINT sync, UINT flags) {
    if (sc != g_sc) return r_present(sc, sync, flags);
    g_presents++;
    if (!g_main_tid) g_main_tid = GetCurrentThreadId();
    if (GetCurrentThreadId() != g_main_tid) g_present_other_thread++;
    g_sync[sync < 4 ? sync : 4]++;
    unsigned ret = (unsigned)(UINT_PTR)__builtin_return_address(0), game = 0;
    game_callers((const unsigned *)__builtin_frame_address(0), &game, 1);
    for (int i = 0; i < 8; i++) {
        if (g_callers[i].n && (g_callers[i].ret != ret || g_callers[i].game_ret != game)) continue;
        g_callers[i].ret = ret; g_callers[i].game_ret = game; g_callers[i].n++;
        break;
    }
    if (g_capturing && g_cap) {
        char t[600];
        set_end("at Present");
        targets(t, sizeof t);
        fprintf(g_cap, "Present(sync %u, flags 0x%x) (game +0x%X), draws %u: %s\n", sync, flags, game, g_draws, t);
        dump_bb("at Present");
    }
    if (g_live_state == 1) {   // the helper presents (started in w_create_sc)
        DXGI_SWAP_CHAIN_DESC d;
        ID3D11Resource *bb = backbuffer();
        if (bb && SUCCEEDED(IDXGISwapChain_GetDesc(sc, &d))) {
            if (d.BufferDesc.Width != g_live_w || d.BufferDesc.Height != g_live_h || !fgb_active()) {
                // shortcut: a resize (or a helper that died) ends the bridge for the session; restart the game to get it back
                g_log("frame bridge: back buffer %ux%u (was %ux%u), helper %s - bridge off until the game restarts", d.BufferDesc.Width,
                      d.BufferDesc.Height, g_live_w, g_live_h, fgb_active() ? "running" : "gone");
                g_off_note = fgb_active() ? L"The game's size changed - restart the game to turn DLSS back on. Choices made here are saved."
                                          : L"DLSS stopped (its helper closed) - restart the game to turn it back on. Choices made here are saved.";
                bridge_off();
            } else {
                LARGE_INTEGER tp;
                QueryPerformanceCounter(&tp);
                if (g_t_last_present && tp.QuadPart - g_t_last_present > g_t_frame_max) g_t_frame_max = tp.QuadPart - g_t_last_present;
                g_t_last_present = tp.QuadPart;
                g_n_frames++;
                jit_frame_end();   // a frame without a HUD-less point
                if (g_view && g_motion && g_hud_frame == g_frame) fl_present(fgb_mv_texture());   // flash probe
#ifdef PROFILING
                if (g_view && g_motion && g_hud_frame == g_frame) r2m_show(bb, g_rw, g_rh);   // this frame's models only; under the panel
#endif
                panel_frame(bb);
                fgb_present(bb);
                if (g_motion) r2m_frame_end();   // a frame without the pass drops its recorded draws
                ID3D11Resource_Release(bb);
                // the game's own swapchain is presented too (hidden window, sync 0): without its Presents the NVIDIA driver
                // stalled the game's D3D11 calls (in game: Map 17.7 ms per frame at 2,093 Maps; Attila AGENT_LOG 109: 90 -> 155 fps)
                dxgi_present(sc, 0, 0);
                g_scene_last = g_hud_frame == g_frame;
                bp_frame();
                jit_frame_start();
                return S_OK;
            }
        }
        if (bb) ID3D11Resource_Release(bb);
    }
    if (sc == g_hid_sc && g_live_state == 2) {   // after the bridge: the frame goes to the window through the mod's swapchain
        ID3D11Resource *bb = backbuffer();
        if (bb) fallback_panel(bb);
        IDXGISwapChain *own = bb ? fgb_fallback_frame(bb, dxgi_create_sc) : NULL;
        if (bb) ID3D11Resource_Release(bb);
        bp_frame();
        if (!own) return S_OK;
        if (!g_fb_frames++) g_log("frame bridge: the game's frames go to its window through the mod's own swapchain");
        return dxgi_present(own, sync, flags);
    }
    bp_frame();
    return r_present(sc, sync, flags);
}

// The helper's flip swapchain needs the game's window to itself (Attila: "Access is denied" next to the game's), so the
// bridge starts before the game's swapchain exists and the game's swapchain goes to a hidden window of the same size.
static HWND hidden_window(UINT w, UINT h) {
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = L"fgb_hidden";
    RegisterClassW(&wc);
    return CreateWindowExW(0, L"fgb_hidden", L"", WS_POPUP, 0, 0, w, h, NULL, NULL, wc.hInstance, NULL);
}
// Exclusive fullscreen on the hidden window would cover the screen with a picture nobody draws (Attila 6.3.1): the game's
// swapchain stays windowed; its fullscreen requests are answered without switching, and it is told the state it asked for.
static BOOL g_fs_wanted;
static HRESULT(STDMETHODCALLTYPE *r_setfs)(IDXGISwapChain *, BOOL, IDXGIOutput *);
static HRESULT(STDMETHODCALLTYPE *r_getfs)(IDXGISwapChain *, BOOL *, IDXGIOutput **);
static HRESULT STDMETHODCALLTYPE w_setfs(IDXGISwapChain *sc, BOOL fs, IDXGIOutput *target) {
    if (sc != g_hid_sc) return r_setfs(sc, fs, target);
    if (fs != g_fs_wanted) g_log("frame bridge: the game switches to %s - kept as a borderless window", fs ? "fullscreen" : "windowed");
    g_fs_wanted = fs;
    return S_OK;
}
// Other mods find the game's window through its swapchain (Attila 6.5: the 4K UI mod's F10 menu showed but took no
// clicks, it subclassed the hidden window, which gets no input). The hidden swapchain says the window the game gave it.
static HRESULT(STDMETHODCALLTYPE *r_getdesc)(IDXGISwapChain *, DXGI_SWAP_CHAIN_DESC *);
static HRESULT(STDMETHODCALLTYPE *r_gethwnd)(IDXGISwapChain1 *, HWND *);
static HRESULT STDMETHODCALLTYPE w_getdesc(IDXGISwapChain *sc, DXGI_SWAP_CHAIN_DESC *d) {
    HRESULT hr = r_getdesc(sc, d);
    if (SUCCEEDED(hr) && d && sc == g_hid_sc && g_out_hwnd) d->OutputWindow = g_out_hwnd;
    return hr;
}
static HRESULT STDMETHODCALLTYPE w_gethwnd(IDXGISwapChain1 *sc, HWND *h) {
    HRESULT hr = r_gethwnd(sc, h);
    if (SUCCEEDED(hr) && h && (IDXGISwapChain *)sc == g_hid_sc && g_out_hwnd) *h = g_out_hwnd;
    return hr;
}
static HRESULT STDMETHODCALLTYPE w_getfs(IDXGISwapChain *sc, BOOL *fs, IDXGIOutput **target) {
    if (sc != g_hid_sc) return r_getfs(sc, fs, target);
    if (fs) *fs = g_fs_wanted;
    if (target) {
        *target = NULL;
        if (g_fs_wanted && FAILED(IDXGISwapChain_GetContainingOutput(sc, target))) *target = NULL;
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE w_create_sc(IDXGIFactory *f, IUnknown *dev, DXGI_SWAP_CHAIN_DESC *d, IDXGISwapChain **out) {
    DXGI_SWAP_CHAIN_DESC dd = *d;
    if (g_live_state == 1) {   // shortcut: a second swapchain (settings change) ends the bridge; restart the game to get it back
        g_log("frame bridge: the game makes a new swapchain (%ux%u) - DLSS off until the game restarts", d->BufferDesc.Width, d->BufferDesc.Height);
        g_off_note = L"Graphics settings changed - restart the game to turn DLSS back on. Choices made here are saved.";
        bridge_off();
    }
    if (g_live && g_live_state == 0) {
        ID3D11Device *nd = NULL;
        if (SUCCEEDED(IUnknown_QueryInterface(dev, &iid_device, (void **)&nd)) && nd != g_dev) {   // the swapchain's device is the game's
            if (g_ctx) ID3D11DeviceContext_Release(g_ctx);
            if (g_dev) ID3D11Device_Release(g_dev);
            g_dev = nd;
            nd = NULL;
            ID3D11Device_GetImmediateContext(g_dev, &g_ctx);
            hook_context();
            mp_device_hooks();
        }
        if (nd) ID3D11Device_Release(nd);
        RECT rc = {0};
        GetClientRect(d->OutputWindow, &rc);
        g_live_w = d->BufferDesc.Width ? d->BufferDesc.Width : (UINT)rc.right;
        g_live_h = d->BufferDesc.Height ? d->BufferDesc.Height : (UINT)rc.bottom;
        HWND hid = NULL;
        g_live_state = g_ctx && (hid = hidden_window(g_live_w, g_live_h)) &&
                               fgb_start(g_dev, g_ctx, d->OutputWindow, g_live_w, g_live_h, d->BufferDesc.Format, g_helper, g_sl, g_logdir, g_fg, g_log)
                           ? 1 : 2;
        if (g_live_state == 1) {
            dd.OutputWindow = hid; dd.BufferDesc.Width = g_live_w; dd.BufferDesc.Height = g_live_h;
            if (!dd.Windowed) {
                dd.Windowed = TRUE;
                g_fs_wanted = TRUE;
                g_log("frame bridge: the game asked for exclusive fullscreen - it runs as a borderless window while the bridge is on");
            }
            g_out_hwnd = d->OutputWindow;
            sr_set(g_sr);   // DLAA / presets: the render size and DLSS (off: native, no DLSS)
            fgb_set_preset(PRESET_NGX[g_preset]);
            fgb_set_sharpness(SHARP_CON[g_sharp]);
            if (!fgb_helper_has_presets())   // the engine fix updated, the add-on not (or a hand install of an older one)
                g_log("DLSS add-on: its helper is older than this engine fix (add-on version 1) - the DLSS preset choice needs "
                      "add-on version 2: update the add-on (Workshop: run its install.bat again; elsewhere: copy its FrameBridge folder)");
            if (g_fg > fgb_fg_max()) g_fg = fgb_fg_max();   // NVIDIA's rules for this GPU (fgb_start logged it)
            g_refresh = refresh_hz(d->OutputWindow);   // the frame cap's Auto (the screen the game window is on)
            cap_apply();
            // per-object motion: set up whenever the bridge runs, so the F11 panel can switch it on (fgbridge_on.txt "motion": on at start)
            int no_fg = !fgb_fg_max();   // RTX 20 / 30: per-object motion mostly serves frame generation (Attila 6.5.2)
            if (no_fg) g_log("motion pass: off - this GPU has no frame generation, which per-object motion is for");
            g_motion = no_fg ? 0 : r2m_init(g_dev, g_ctx, fgb_mv_texture(), g_log);
            if (g_motion) { fgb_set_motion(motion_cb); r2m_set_enabled(g_motion_on); }
            FgpApply ap = {panel_sr, panel_fg, panel_motion, PANEL_VIEW, panel_cap, panel_preset, PANEL_JTEST, panel_sharp};
            if (fgp_init(g_dev, g_ctx, g_live_w, g_live_h, d->BufferDesc.Format, g_sr, (int)g_fg, g_motion ? g_motion_on : no_fg ? -2 : -1, ap)) {
                fgp_set_sr_choices(6);
                fgp_set_preset(g_preset);
                fgp_set_sharp(g_sharp);
                fgp_set_presets_available(fgb_helper_has_presets());
            }
            else g_log("frame bridge: F11 panel not available (back buffer format %u)", d->BufferDesc.Format);
        } else if (hid) DestroyWindow(hid);
    }
    // after the bridge a swapchain on the game's window is never shown (fgbridge.c, fallback): the game's new one goes to a
    // hidden window too and its frames reach the window through the mod's swapchain
    if (g_live_state == 2 && g_out_hwnd && d->OutputWindow == g_out_hwnd) {
        RECT rc = {0};
        GetClientRect(d->OutputWindow, &rc);
        UINT w = d->BufferDesc.Width ? d->BufferDesc.Width : (UINT)rc.right, h = d->BufferDesc.Height ? d->BufferDesc.Height : (UINT)rc.bottom;
        HWND hid = w && h ? hidden_window(w, h) : NULL;
        if (hid) {
            dd.OutputWindow = hid; dd.BufferDesc.Width = w; dd.BufferDesc.Height = h;
            g_fs_wanted = !d->Windowed;
            dd.Windowed = TRUE;
        }
    }
    fgb_fallback_close();   // ours belongs to the previous swapchain
    g_hid_sc = NULL;
    HRESULT hr = r_create_sc(f, dev, &dd, out);
    int hidden = SUCCEEDED(hr) && dd.OutputWindow != d->OutputWindow;
    if (FAILED(hr) && g_live_state == 1) { g_log("frame bridge: the game's swapchain on the hidden window failed (0x%08lx) - bridge off", hr); bridge_off(); }
    if (FAILED(hr) && dd.OutputWindow != d->OutputWindow) hr = r_create_sc(f, dev, d, out);
    g_log("bridge probe: CreateSwapChain %ux%u format %u buffers %u swap effect %u windowed %d flags 0x%x sample count %u -> 0x%08lx%s%s",
          d->BufferDesc.Width, d->BufferDesc.Height, d->BufferDesc.Format, d->BufferCount, d->SwapEffect, d->Windowed, d->Flags,
          d->SampleDesc.Count, hr, g_sc ? " (a later swapchain: the probe follows it)" : "", hidden ? ", on the bridge's hidden window" : "");
    if (SUCCEEDED(hr) && out && *out) {
        g_sc = *out;   // shortcut: the newest swapchain is the game's (a resize recreates buffers, not the swapchain)
        if (hidden) g_hid_sc = *out;
        if (g_staging) { ID3D11Texture2D_Release(g_staging); g_staging = NULL; }
        void **vt = *(void ***)*out;
        void **sl = &vt[offsetof(IDXGISwapChainVtbl, Present) / sizeof(void *)];
        // DXGI slots are hooked once (all swapchains share the vtable): a mod hooked on top of ours calls ours as its
        // original, so hooking again would make ours call it back forever (Attila 6.3, a new swapchain in game)
        if (!r_present) {
            r_present = *sl;
            patch_slot(sl, (void *)w_present);
            g_present_real = dxgi_own(vt, offsetof(IDXGISwapChainVtbl, Present) / sizeof(void *), (void *)r_present, "Present");
        }
        if (hidden && !r_setfs) {
            sl = &vt[offsetof(IDXGISwapChainVtbl, SetFullscreenState) / sizeof(void *)];
            r_setfs = *sl;
            patch_slot(sl, (void *)w_setfs);
            sl = &vt[offsetof(IDXGISwapChainVtbl, GetFullscreenState) / sizeof(void *)];
            r_getfs = *sl;
            patch_slot(sl, (void *)w_getfs);
            sl = &vt[offsetof(IDXGISwapChainVtbl, GetDesc) / sizeof(void *)];
            r_getdesc = *sl;
            patch_slot(sl, (void *)w_getdesc);
            static const GUID iid_sc1 = {0x790a45f7, 0x0d42, 0x4876, {0x98, 0x3a, 0x0a, 0x55, 0xcf, 0xe6, 0xf4, 0xaa}};
            IDXGISwapChain1 *s1 = NULL;   // GetHwnd: only where IDXGISwapChain1 is the same object (same table, one more slot)
            if (SUCCEEDED(IDXGISwapChain_QueryInterface(*out, &iid_sc1, (void **)&s1))) {
                if ((void *)s1 == (void *)*out) {
                    sl = &vt[offsetof(IDXGISwapChain1Vtbl, GetHwnd) / sizeof(void *)];
                    r_gethwnd = *sl;
                    patch_slot(sl, (void *)w_gethwnd);
                }
                IDXGISwapChain1_Release(s1);
            }
        }
    }
    return hr;
}

typedef HRESULT(WINAPI *factory_fn)(REFIID, void **);
static factory_fn r_factory;
static HRESULT WINAPI w_factory(REFIID r, void **out) {
    HRESULT hr = r_factory(r, out);
    if (SUCCEEDED(hr) && out && *out) {
        void **vt = *(void ***)*out;
        void **sl = &vt[offsetof(IDXGIFactoryVtbl, CreateSwapChain) / sizeof(void *)];
        if (!r_create_sc) {
            r_create_sc = *sl;
            patch_slot(sl, (void *)w_create_sc);
            g_create_real = dxgi_own(vt, offsetof(IDXGIFactoryVtbl, CreateSwapChain) / sizeof(void *), (void *)r_create_sc, "CreateSwapChain");
        }
    }
    return hr;
}

typedef HRESULT(WINAPI *create_fn)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT,
                                   ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
static create_fn r_create;
static HRESULT WINAPI w_create(IDXGIAdapter *a, D3D_DRIVER_TYPE t, HMODULE sw, UINT flags, const D3D_FEATURE_LEVEL *fl, UINT nfl,
                               UINT sdk, ID3D11Device **dev, D3D_FEATURE_LEVEL *got, ID3D11DeviceContext **ctx) {
    HRESULT hr = r_create(a, t, sw, flags, fl, nfl, sdk, dev, got, ctx);
    g_creates++;
    g_log("bridge probe: D3D11CreateDevice #%d flags 0x%x -> 0x%08lx, feature level 0x%x", g_creates, flags, hr, got ? *got : 0);
    // the first device only (Attila: video / overlay devices appear later and are not the game's)
    if (SUCCEEDED(hr) && dev && *dev && !g_ctx) {
        g_dev = *dev;
        ID3D11Device_AddRef(g_dev);
        ID3D11Device_GetImmediateContext(g_dev, &g_ctx);
        hook_context();
        mp_device_hooks();   // before the game creates its shaders
    }
    return hr;
}

static FARPROC(WINAPI *r_gpa)(HMODULE, LPCSTR);
static FARPROC WINAPI w_gpa(HMODULE m, LPCSTR name) {
    FARPROC f = r_gpa(m, name);
    if (!f || (UINT_PTR)name <= 0xFFFF) return f;
    if (!strcmp(name, "D3D11CreateDevice")) { r_create = (create_fn)(void *)f; return (FARPROC)(void *)w_create; }
    if (!strcmp(name, "CreateDXGIFactory1")) { r_factory = (factory_fn)(void *)f; return (FARPROC)(void *)w_factory; }
    return f;
}

static int has_word(const char *line, const char *w) {   // w as a whole space-separated word of line
    size_t n = strlen(w);
    for (const char *p = strstr(line, w); p; p = strstr(p + 1, w))
        if ((p == line || p[-1] == ' ') && (!p[n] || p[n] == ' ')) return 1;
    return 0;
}

// m: empire.retail.dll (before its startup code runs); dir: the game folder (no trailing backslash)
int install_bridgeprobe(HMODULE m, const wchar_t *dir, void (*log)(const char *fmt, ...)) {
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((unsigned char *)m + ((IMAGE_DOS_HEADER *)m)->e_lfanew);
    g_base = (unsigned)(UINT_PTR)m;
    g_size = nt->OptionalHeader.SizeOfImage;
    g_log = log;
    wcsncpy(g_dir, dir, MAX_PATH - 1);
    void *old = iat_hook(m, "KERNEL32.dll", "GetProcAddress", (void *)w_gpa);
    if (!old) { log("bridge probe: GetProcAddress import not found - not installed"); return 0; }
    r_gpa = (FARPROC(WINAPI *)(HMODULE, LPCSTR))old;
    wchar_t p[MAX_PATH], addon[MAX_PATH];
    wcscpy(g_logdir, g_dir);
    _snwprintf(p, MAX_PATH, L"%ls\\fgbridge_on.txt", g_dir);
    FILE *f = _wfopen(p, L"r");
    _snwprintf(addon, MAX_PATH, L"%ls\\FrameBridge", g_dir);
    _snwprintf(p, MAX_PATH, L"%ls\\helper64.exe", addon);
    if (!f && GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) {
        // the release add-on (as Attila's, its AGENT_LOG 104): <game>\FrameBridge\ holds helper64.exe, NVIDIA's Streamline /
        // DLSS DLLs (the helper loads them from there after checking NVIDIA's signature), settings.txt and the helper's logs
        wcscpy(g_helper, p);
        wcscpy(g_sl, addon);
        wcscpy(g_logdir, addon);
        _snwprintf(g_settings, MAX_PATH, L"%ls\\settings.txt", addon);
        char text[2048] = {0};
        FILE *sf = _wfopen(g_settings, L"r");
        if (sf) { fread(text, 1, sizeof text - 1, sf); fclose(sf); }
        static const char *const MV_KEYS[2] = {"camera_only", "per_object"};
        g_sr = settings_value(text, "super_resolution=", SR_KEYS, 6, 2);                 // default Quality (faster big battles)
        g_fg = (unsigned)settings_value(text, "frame_generation=", FG_KEYS, 4, 1);   // default 2x (limited to the GPU later)
        g_motion_on = settings_value(text, "motion_vectors=", MV_KEYS, 2, 0);            // default camera only
        g_fcap = settings_value(text, "frame_cap=", CAP_KEYS, 5, 1);                       // default auto
        g_preset = settings_value(text, "dlss_preset=", PRESET_KEYS, 5, 0);                // default: NVIDIA's per mode
        g_sharp = settings_value(text, "sharpness=", SHARP_KEYS, 4, 1);                     // default low
        g_live = 1;
        g_dev_di8_created = fgp_di8_created;   // the panel hides the game's mouse buttons while the cursor is over it
        log("DLSS add-on found: super resolution %s, frame generation %s, motion vectors %s, frame cap %s, preset %s, sharpness %s (%ls)", SR_KEYS[g_sr],
            FG_KEYS[g_fg], MV_KEYS[g_motion_on], CAP_KEYS[g_fcap], PRESET_KEYS[g_preset], SHARP_KEYS[g_sharp], sf ? L"settings.txt" : L"defaults");
        if (!sf) settings_save();   // the file to edit, with its comments, from the first start on
    }
    if (f) {   // helper64.exe path, Streamline dir, frames to generate (0..3), options ("fakeamd": treat the GPU as not NVIDIA)
        char l[4][MAX_PATH] = {{0}};
        for (int i = 0; i < 4 && fgets(l[i], MAX_PATH, f); i++) l[i][strcspn(l[i], "\r\n")] = 0;
        fclose(f);
        MultiByteToWideChar(CP_ACP, 0, l[0], -1, g_helper, MAX_PATH);
        MultiByteToWideChar(CP_ACP, 0, l[1], -1, g_sl, MAX_PATH);
        memcpy(g_on_lines, l, sizeof g_on_lines);
        g_mp = strstr(l[3], "motionprobe0") ? 2 : strstr(l[3], "motionprobe") ? 1 : 0;   // the motion probe (G1)
        snprintf(g_on_test, sizeof g_on_test, "%s%s%s", strstr(l[3], "fakeamd") ? "fakeamd" : "", strstr(l[3], "fakeamd") && g_mp ? " " : "",
                 g_mp == 2 ? "motionprobe0" : g_mp ? "motionprobe" : "");   // kept when the panel saves the choices
        g_fg = (unsigned)atoi(l[2]);
        g_live = g_helper[0] && g_sl[0] && g_fg <= 3;
        if (strstr(l[3], "fakeamd")) { fgb_set_test_not_nvidia(1); log("  test: GPU treated as not NVIDIA (bridge stays off)"); }
        g_sr = strstr(l[3], "ultraperf") ? 5 : strstr(l[3], "perf") ? 4 : strstr(l[3], "balanced") ? 3 : strstr(l[3], "quality") ? 2
             : strstr(l[3], "dlaa") ? 1 : 0;   // the F11 panel switches it
        g_motion_on = has_word(l[3], "motion");   // the word, not "motionprobe"
        for (int k = 0; k < 5; k++) {   // capoff / cap60 / cap90 / cap120; none: auto
            char t[16];
            snprintf(t, sizeof t, "cap%s", CAP_KEYS[k]);
            if (k != 1 && has_word(l[3], t)) g_fcap = k;
        }
        for (int k = 0; k < 4; k++) {   // sharpoff / sharpmedium / sharphigh; none: low
            char t[16];
            snprintf(t, sizeof t, "sharp%s", SHARP_KEYS[k]);
            if (k != 1 && has_word(l[3], t)) g_sharp = k;
        }
        for (int k = 1; k < 5; k++) {   // presetj / presetk / presetl / presetm; none: default
            char t[16];
            snprintf(t, sizeof t, "preset%s", PRESET_KEYS[k]);
            if (has_word(l[3], t)) g_preset = k;
        }
        if (g_live) g_dev_di8_created = fgp_di8_created;   // the panel hides the game's mouse buttons while the cursor is over it
        log("frame bridge %s: helper %s, Streamline %s, frame generation %ux, super resolution %s", g_live ? "armed" : "NOT armed (bad fgbridge_on.txt)",
            l[0], l[1], g_fg + 1, SR_NAMES[g_sr]);
    }
    if (!g_settings[0]) log("bridge probe on: D3D11 device / swapchain watched; bridge_go.txt in the game folder captures the next frame");
    return 1;
}
