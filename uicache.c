// Battle UI cache (from Attila 7.1, its heapfix/uicache.c and notes/ui_cache.md): in a battle the unit cards are not
// walked and drawn every frame. Their panel is drawn into two textures a few times a second and on input, and shown
// from those textures on the frames between. Attila: 0.4 to 1 ms a frame less in its battles (runs 3 to 6 there).
//
// TWUI::Component::Draw (the Linux build's name in Attila; Rome 2 has no RTTI names for TWUI) is one function for the
// whole tree: it calls itself for the children through slot 1 of the vtable. It is hooked, and for a named panel
//   - on a redraw the game draws it into two textures of ours instead of the screen: one cleared to black, one to
//     white, every draw sent to both (our patch on d3d11's draw functions repeats it), the game's own blend states
//     untouched. As long as each draw leaves "what was there times something plus something" (over, additive,
//     multiply), the black one then holds what the panel adds and white minus black what it lets through;
//   - every frame one draw puts that over the scene: scene x (white - black) + black, with dual-source blending.
// Blending that reads the target's alpha, or takes a minimum / maximum, cannot be stood in for: a panel that uses it is
// drawn the game's way from then on (said in the log).
// The textures are the size of the target, so each draw lands in them where it would have landed on the target; the
// paste covers only the panel's box, which is taken from what it draws: the records it flushes (see uic_flush). Not from
// the component's position and size fields: in game (2026-11-25, ui_scale 1.7 at 4K) those are layout units the game
// scales again in its projection (x 1.69 there), and a cache placed by them held nothing - the unit cards were missing
// in 2.2 and in two 2.3 test builds; two probe battles measured it (AGENT_LOG).
// Redrawn: when there is nothing yet; every INTERVAL ms; while the cursor is over it; for 250 ms after a mouse button or
// a non-camera key goes down or comes up (held, it does not count: Attila 7.1); when the game hands it other draw
// details, or it has moved or changed size.
//
// Rome 2's Draw (capstone 2026-11-23, the same in all three known builds apart from register choice and addresses):
// starts with "mov eax, [abs]" (5 bytes), ret 0x10, returns 1, Draw(DrawDetails &, unsigned, bool, bool) as in Attila.
// The component's fields are Attila's minus 0xC (name, parent, visible, state) - its position is read from +0x94 (the
// absolute one: Draw culls the children against the clip rect with it, and +0x1057C70 (Steam) sets it to the parent's
// plus the relative position at +0x8C). Position and size only tell whether the panel moved (a redraw); they are not its
// pixels (above).
//
// Nothing stands between the game and its Draw: it is entered with the stack and every register as the game set them;
// only when the panel is to be redrawn is its return address swapped for ours, and when it is shown from the textures
// the function is not entered at all (as a hidden component is not: Draw's own early exit).
// d3d11's functions are patched only when the panel is first redrawn (in a battle), each patch a jump through a pointer
// that leads straight on except while the panel is being redrawn. The DLSS add-on keeps hooks in the context's vtable
// (bridgeprobe.c): d3d11's own function is taken from behind them (fgb_d3d11_original), and what the cache sets for itself
// for a moment goes straight to d3d11, so the add-on sees the game's calls only (Attila 7f46316).
//
// The game does not draw a UI element when its component is drawn: it gathers quads in a sprite batch (4,000 vertices,
// count at +0x714E4, ring offset +0x714EC: Rome 2's flush has the same layout as Attila's) and sends them when the next
// element has another state. The batch is flushed with the game's own function at both edges of the panel (Attila run 4:
// without it the last card's bar and the button before the panel flickered).
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "builds.h"

extern volatile LONG g_frame_no;   // fpslog.c: counts only while the tile scan's frame hook is in
void *__cdecl fgb_d3d11_original(unsigned slot) __attribute__((weak));   // bridgeprobe.c: NULL when the slot is not the add-on's

#define C_NAME 0x78     // char *: the component's name (a CA::String at +0x70, its characters at +8: "Child %d of component %s")
#define C_PARENT 0x7C   // Draw: no parent = the root, which sets the clip rect
#define C_POS 0x94      // float x, y in the UI's units, absolute
#define C_STATE 0xA8    // its current state; width at +0x28, height at +0x2C (the game's Width / Height read them so)
#define C_VISIBLE 0xE5  // Draw returns at once when 0
#define R_DEVICE 0x138  // the D3D11 renderer's ID3D11Device (its D3D11CreateDevice call writes it there)
#define DETAILS 0x38    // bytes of the DrawDetails compared (Draw copies +0..+0x34 into the children's)
#define LEN_FLUSH 0x80
enum { NTARGET = 4, NBLEND = 32, NFN = 16, INTERVAL = 66 };

// per known build (builds.h order). All hashes are FNV-1a of the code as on disk (relocations undone), in 0x2F0 pieces.
//   draw, its length, hash; where in it the clip rect's address is (the root's "mov [clip x], 0");
//   the sprite batch's flush, hash of its first LEN_FLUSH bytes (with the renderer global's address at +0x30);
//   the D3D11 renderer's vtable (its factory allocates 0x1BC8 bytes and its constructor writes this table), the method in
//   its slot 28 that calls D3D11CreateDevice with &this[R_DEVICE], how many of its bytes are hashed (to past that), hash
static const DWORD g_uc_builds[NBUILDS][10] = {
    {0x1048FB0, 0x976, 0x7AE58371u, 0xA7, 0x1457FC0, 0x535F987Cu, 0x191D444, 0x14907F0, 0x0F0, 0x0BB4E9FDu},   // Steam 69CE2041
    {0x1024780, 0x97C, 0x685EAE7Fu, 0x9B, 0x1431B60, 0xC2BA4661u, 0x18C16D0, 0x1469FF0, 0x0F0, 0x2D635C19u},   // pantheon beta 6AB978BD
    {0x1048FB0, 0x976, 0xB5C4BD91u, 0xA7, 0x1457DB0, 0x535F987Cu, 0x191D4A4, 0x14905F0, 0x0F0, 0x0BB4E9FDu},   // GOG 6A564E92
};
enum { B_DRAW, B_DRAW_LEN, B_DRAW_FNV, B_CLIP_AT, B_FLUSH, B_FLUSH_FNV, B_VT, B_CREATE, B_CREATE_LEN, B_CREATE_FNV };

typedef struct {
    char name[48];
    const char *under;             // an ancestor it must have (NULL: any)
    void *comp;                    // the component last seen under this name
    ID3D11Texture2D *tex[2];       // drawn over black, drawn over white
    ID3D11RenderTargetView *rtv[2];
    ID3D11ShaderResourceView *srv[2];
    unsigned tw, th;               // the textures' size (the target's, in steps of 64)
    unsigned tgt_w, tgt_h;         // the target's size
    int box[4];                    // the panel's pixels on the target: x0, y0, x1, y1 (x1 <= x0: it drew nothing there)
    float rect[4];                 // the component's x, y, width, height when it was last drawn
    int valid, off;                // off: something in it cannot be cached, it is drawn the game's way from then on
    DWORD drawn;                   // GetTickCount of the last redraw
    LONG seen;                     // frame it was last drawn in
    unsigned char details[DETAILS];
    unsigned why[6], reuse;        // redraws by reason (1 first, 2 moved / details, 3 a key or button, 4 interval, 5 the cursor over it), frames shown from the textures
    int said;                      // its first redraw has been written to the log
} Target;
static Target g_t[NTARGET];
static int g_nt;

static void (*g_log)(const char *fmt, ...);
static unsigned char *g_base;      // the game module; NULL in tests
static const DWORD *g_build;       // its table entry
static unsigned char **g_renderer; // the global holding the renderer
static int g_on = 1;
static unsigned char *g_stubs;     // the entry stub, at +0x28 the one a redrawn panel's Draw returns into, at +0x40 the flush's
void *g_uic_ret;                   // where that Draw was to return to
void *g_uic_batch;                 // the batch the game last flushed this frame (the stub at the flush's start notes it)
static void(__fastcall *g_flush)(void *batch, void *edx);
static unsigned g_nobatch;         // times nothing could be flushed: no flush of the game's seen yet that frame
static void flush_pending(void) {
    void *b = g_uic_batch;
    if (b && g_flush) g_flush(b, NULL);
    else g_nobatch++;
    (void)g_nobatch;   // read by the profiling build's log only
}

static IUnknown *g_dev_seen;       // the renderer's device as last read
static ID3D11Device *g_dev;
static ID3D11DeviceContext *g_ctx;
static ID3D11VertexShader *g_vs;
static ID3D11PixelShader *g_ps;
static ID3D11BlendState *g_blend;
static ID3D11DepthStencilState *g_dss;
static ID3D11RasterizerState *g_rs;
static struct { ID3D11BlendState *bs; int ok; } g_seen[NBLEND];
static int g_nseen, g_tried;

// d3d11's functions, hot-patched
enum { F_RT, F_VP, F_DRAW, F_DRAWI, F_DRAWN, F_DRAWIN, F_KINDS };
static const unsigned short g_slotof[F_KINDS] = {
    offsetof(ID3D11DeviceContextVtbl, OMSetRenderTargets) / sizeof(void *), offsetof(ID3D11DeviceContextVtbl, RSSetViewports) / sizeof(void *),
    offsetof(ID3D11DeviceContextVtbl, Draw) / sizeof(void *),               offsetof(ID3D11DeviceContextVtbl, DrawIndexed) / sizeof(void *),
    offsetof(ID3D11DeviceContextVtbl, DrawInstanced) / sizeof(void *),      offsetof(ID3D11DeviceContextVtbl, DrawIndexedInstanced) / sizeof(void *)};
static void *g_fn[NFN], *g_next[NFN];
static void(STDMETHODCALLTYPE *g_set_rt)(ID3D11DeviceContext *, UINT, ID3D11RenderTargetView *const *, ID3D11DepthStencilView *);
static void(STDMETHODCALLTYPE *g_set_vp)(ID3D11DeviceContext *, UINT, const D3D11_VIEWPORT *);
void *g_uic_go[NFN];               // where each patched function's jump leads: on into d3d11, or to our stub while a panel is redrawn
static unsigned char g_fkind[NFN];
static int g_nfn, g_patch_failed;
static unsigned char *g_thunks;    // 48 bytes a function: jmp [g_uic_go[i]], and at +8 the stub that calls uic_d3d
static uintptr_t g_d3d_lo, g_d3d_size;

// while the game draws a panel into its textures
static Target *g_red;
static ID3D11RenderTargetView *g_bb, *g_game_rt;   // the target it was drawing to (held); the one it last asked for
static ID3D11DepthStencilView *g_dsv;
static D3D11_VIEWPORT g_game_vp;                   // the viewport it last asked for
static int g_red_bad, g_own;                       // g_own: the D3D calls are ours

#define REL(p) do { if (p) { IUnknown_Release((IUnknown *)(p)); (p) = NULL; } } while (0)

static const char *hlsl =
    "struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
    "V vs(uint i : SV_VertexID) { V o; o.uv = float2((i << 1) & 2, i & 2); o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
    "Texture2D black : register(t0); Texture2D white : register(t1);\n"
    "struct O { float4 add : SV_Target0; float4 keep : SV_Target1; };\n"
    "O ps(V i) { int3 at = int3(i.p.xy, 0);\n"
    "  float3 b = black.Load(at).rgb, wh = white.Load(at).rgb; O o; o.add = float4(b, 1); o.keep = float4(max(wh - b, 0), 1); return o; }\n";

// Everything made on the device is let go (the game made a new one, or there is none yet).
static void drop_device(void) {
    for (int i = 0; i < g_nt; i++) {
        for (int k = 0; k < 2; k++) { REL(g_t[i].srv[k]); REL(g_t[i].rtv[k]); REL(g_t[i].tex[k]); }
        g_t[i].valid = 0; g_t[i].tw = g_t[i].th = 0;
    }
    REL(g_vs); REL(g_ps); REL(g_blend); REL(g_dss); REL(g_rs); REL(g_ctx); REL(g_dev);
    g_nseen = g_tried = 0;
}

// The game's context and our own objects on its device. NULL: nothing can be cached now.
static ID3D11DeviceContext *device(void) {
    if (g_base) {
        unsigned char *r = *g_renderer;
        // the D3D11 renderer only (the factory's other one, when D3D11 cannot be made, has something else there)
        IUnknown *d = r && *(unsigned char **)r == g_base + g_build[B_VT] ? *(IUnknown **)(r + R_DEVICE) : NULL;
        if (d != g_dev_seen) {
            drop_device();
            g_dev_seen = d;
            static const GUID iid_dev = {0xdb6f6ddb, 0xac77, 0x4e88, {0x82, 0x53, 0x81, 0x9d, 0xf9, 0xbb, 0xf1, 0x40}};   // ID3D11Device
            if (d && SUCCEEDED(IUnknown_QueryInterface(d, &iid_dev, (void **)&g_dev)) && g_dev) ID3D11Device_GetImmediateContext(g_dev, &g_ctx);
        }
    } else if (g_ctx && !g_dev) ID3D11DeviceContext_GetDevice(g_ctx, &g_dev);   // tests set g_ctx
    if (!g_ctx || !g_dev) return NULL;
    if (g_tried) return g_vs && g_ps && g_blend && g_dss && g_rs ? g_ctx : NULL;
    g_tried = 1;   // once per device
    pD3DCompile comp = (pD3DCompile)(void *)GetProcAddress(LoadLibraryExA("d3dcompiler_47.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32), "D3DCompile");
    ID3DBlob *vb = NULL, *pb = NULL;
    if (comp && SUCCEEDED(comp(hlsl, strlen(hlsl), "uicache", NULL, NULL, "vs", "vs_4_0", 0, 0, &vb, NULL)) &&
        SUCCEEDED(comp(hlsl, strlen(hlsl), "uicache", NULL, NULL, "ps", "ps_4_0", 0, 0, &pb, NULL))) {
        ID3D11Device_CreateVertexShader(g_dev, ID3D10Blob_GetBufferPointer(vb), ID3D10Blob_GetBufferSize(vb), NULL, &g_vs);
        ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(pb), ID3D10Blob_GetBufferSize(pb), NULL, &g_ps);
    }
    REL(vb); REL(pb);
    D3D11_BLEND_DESC bd = {0};   // scene x keep + add; the target's alpha is left alone
    bd.RenderTarget[0] = (D3D11_RENDER_TARGET_BLEND_DESC){TRUE, D3D11_BLEND_ONE, D3D11_BLEND_SRC1_COLOR, D3D11_BLEND_OP_ADD, D3D11_BLEND_ZERO,
                                                          D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE};
    ID3D11Device_CreateBlendState(g_dev, &bd, &g_blend);
    D3D11_DEPTH_STENCIL_DESC dd = {0};
    ID3D11Device_CreateDepthStencilState(g_dev, &dd, &g_dss);
    D3D11_RASTERIZER_DESC rd = {D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 0, 0, 0, TRUE, FALSE, FALSE, FALSE};
    ID3D11Device_CreateRasterizerState(g_dev, &rd, &g_rs);
    int ok = g_vs && g_ps && g_blend && g_dss && g_rs;
    if (g_log) g_log("battle UI cache: its shaders and states %s", ok ? "made" : "NOT made (no d3dcompiler_47?) - nothing is cached");
    return ok ? g_ctx : NULL;
}

// 1 if what a draw with this blend state leaves is "what was there x something + something" in colour.
static int blend_ok(ID3D11BlendState *bs) {
    if (!bs) return 1;   // no blending: the draw's own colour
    for (int i = 0; i < g_nseen; i++) if (g_seen[i].bs == bs) return g_seen[i].ok;
    D3D11_BLEND_DESC d;
    ID3D11BlendState_GetDesc(bs, &d);
    const D3D11_RENDER_TARGET_BLEND_DESC *r = &d.RenderTarget[0];
#define READS_ALPHA(f) ((f) == D3D11_BLEND_DEST_ALPHA || (f) == D3D11_BLEND_INV_DEST_ALPHA || (f) == D3D11_BLEND_SRC_ALPHA_SAT)
    int ok = !r->BlendEnable || (!READS_ALPHA(r->SrcBlend) && !READS_ALPHA(r->DestBlend) && r->BlendOp != D3D11_BLEND_OP_MIN && r->BlendOp != D3D11_BLEND_OP_MAX);
#undef READS_ALPHA
    if (g_nseen < NBLEND) { g_seen[g_nseen].bs = bs; g_seen[g_nseen].ok = ok; g_nseen++; }
    if (g_log) g_log("battle UI cache: blend state %p (on %d, colour %d/%d op %d, alpha %d/%d op %d, mask %x)%s", (void *)bs, r->BlendEnable, r->SrcBlend, r->DestBlend,
                     r->BlendOp, r->SrcBlendAlpha, r->DestBlendAlpha, r->BlendOpAlpha, r->RenderTargetWriteMask,
                     ok ? "" : " CANNOT be cached: a panel that uses it is drawn the game's way");
    return ok;
}

// The panel's box while it is redrawn: the extent of the records it flushes onto its target, in the target's pixels.
static float g_bx0, g_by0, g_bx1, g_by1;
static unsigned g_brec;

#ifdef PROFILING
// The probe (profiling build, 2026-11-25: the unit cards were not shown in game with the cache, and what the game does
// had to be recorded, not reasoned out). At the panel's first redraw: the raw DrawDetails of the root and of the panel
// and the clip rect. For its first 3 redraws: every target and viewport the game sets, the scissor state at each draw,
// the draws that went into the textures and elsewhere. After the 3rd: the two textures and the target as BMP files in
// the game folder; on the next frame the panel drawn the game's way and the target saved again.
static int g_pr_state;             // 0 waiting, 1 recording redraws, 3 the game's-way frame next, 4 done
static int g_pr_redraws, g_pr_after;
static unsigned char g_pr_root[DETAILS];
static struct {
    unsigned in, out, rts, vps, sc_on, sc_off;
    int nvp, nsc, nrt;
    D3D11_VIEWPORT vp[8];
    D3D11_RECT sc[8];
    void *rt[8];
} g_pr;
static float pr_half(unsigned short h) {
    unsigned e = (h >> 10) & 31, m = h & 1023;
    float v = e ? (float)(m | 1024) * (float)(1u << e) / (float)(1u << 25) : (float)m / (float)(1u << 24);
    return h & 0x8000 ? -v : v;
}
static unsigned char pr_byte(float v) { return v <= 0 ? 0 : v >= 1 ? 255 : (unsigned char)(v * 255 + 0.5f); }
// A texture of the context's device as a 24-bit BMP beside rome2.exe (RGBA8 / BGRA8 or RGBA16F).
static void pr_save(ID3D11DeviceContext *c, ID3D11Resource *res, const char *name) {
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D *s = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &d);
    DXGI_FORMAT fmt = d.Format;
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0; d.MipLevels = 1; d.ArraySize = 1;
    if (d.SampleDesc.Count != 1 || FAILED(ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &s))) { if (g_log) g_log("probe: %s not saved (format %d, %u samples)", name, fmt, d.SampleDesc.Count); return; }
    ID3D11DeviceContext_CopySubresourceRegion(c, (ID3D11Resource *)s, 0, 0, 0, 0, res, 0, NULL);
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    char *sl = n ? strrchr(path, '\\') : NULL;
    if (!sl || (size_t)(sl - path) + 1 + strlen(name) >= MAX_PATH) { REL(s); return; }
    strcpy(sl + 1, name);
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE && SUCCEEDED(ID3D11DeviceContext_Map(c, (ID3D11Resource *)s, 0, D3D11_MAP_READ, 0, &m))) {
        unsigned row = (d.Width * 3 + 3) & ~3u;
        unsigned char hd[54] = {'B', 'M'};
        *(DWORD *)(hd + 2) = 54 + row * d.Height; *(DWORD *)(hd + 10) = 54; *(DWORD *)(hd + 14) = 40;
        *(LONG *)(hd + 18) = (LONG)d.Width; *(LONG *)(hd + 22) = -(LONG)d.Height; *(WORD *)(hd + 26) = 1; *(WORD *)(hd + 28) = 24;
        DWORD w;
        WriteFile(h, hd, 54, &w, NULL);
        unsigned char *line = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, row);
        for (unsigned y = 0; line && y < d.Height; y++) {
            const unsigned char *p = (const unsigned char *)m.pData + (size_t)y * m.RowPitch;
            for (unsigned x = 0; x < d.Width; x++) {
                unsigned char r, g, b;
                if (fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                    const unsigned short *q = (const unsigned short *)p + x * 4;
                    r = pr_byte(pr_half(q[0])); g = pr_byte(pr_half(q[1])); b = pr_byte(pr_half(q[2]));
                } else if (fmt == DXGI_FORMAT_B8G8R8A8_UNORM || fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || fmt == DXGI_FORMAT_B8G8R8X8_UNORM) {
                    b = p[x * 4]; g = p[x * 4 + 1]; r = p[x * 4 + 2];
                } else { r = p[x * 4]; g = p[x * 4 + 1]; b = p[x * 4 + 2]; }
                line[x * 3] = b; line[x * 3 + 1] = g; line[x * 3 + 2] = r;
            }
            WriteFile(h, line, row, &w, NULL);
        }
        if (line) HeapFree(GetProcessHeap(), 0, line);
        ID3D11DeviceContext_Unmap(c, (ID3D11Resource *)s, 0);
        if (g_log) g_log("probe: %s saved (%ux%u, format %d)", name, d.Width, d.Height, fmt);
    }
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    REL(s);
}
// the target bound now, saved
static void pr_save_target(ID3D11DeviceContext *c, const char *name) {
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11Resource *res = NULL;
    ID3D11DeviceContext_OMGetRenderTargets(c, 1, &rtv, NULL);
    if (rtv) ID3D11RenderTargetView_GetResource(rtv, &res);
    if (res) pr_save(c, res, name);
    else if (g_log) g_log("probe: %s not saved (no target bound)", name);
    REL(res); REL(rtv);
}
static void pr_details(const char *what, const void *d) {
    const float *f = d;
    const unsigned *u = d;
    if (!g_log) return;
    g_log("probe: %s DrawDetails as floats: %g %g %g %g %g %g %g %g %g %g %g %g %g %g", what, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13]);
    g_log("probe: %s DrawDetails as hex: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x", what, u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10],
          u[11], u[12], u[13]);
}
// a call of the game's while the panel is redrawn, as it came (before the cache changes anything)
static void pr_event(int kind, void **sp) {
    ID3D11DeviceContext *c = sp[1];
    if (kind == F_RT) {
        ID3D11RenderTargetView **v = sp[3];
        void *rt = (unsigned)(uintptr_t)sp[2] && v ? (void *)v[0] : NULL;
        g_pr.rts++;
        if (g_pr.nrt < 8) g_pr.rt[g_pr.nrt++] = rt;
    } else if (kind == F_VP) {
        const D3D11_VIEWPORT *v = sp[3];
        g_pr.vps++;
        if ((unsigned)(uintptr_t)sp[2] && v) {
            int i = 0;
            while (i < g_pr.nvp && memcmp(&g_pr.vp[i], v, sizeof *v)) i++;
            if (i == g_pr.nvp && g_pr.nvp < 8) g_pr.vp[g_pr.nvp++] = v[0];
        }
    } else {
        if (g_game_rt == g_bb) g_pr.in++; else g_pr.out++;
        ID3D11RasterizerState *rs = NULL;
        D3D11_RASTERIZER_DESC rd = {0};
        ID3D11DeviceContext_RSGetState(c, &rs);
        if (rs) ID3D11RasterizerState_GetDesc(rs, &rd);
        REL(rs);
        if (!rd.ScissorEnable) { g_pr.sc_off++; return; }
        g_pr.sc_on++;
        D3D11_RECT r[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
        UINT n = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        ID3D11DeviceContext_RSGetScissorRects(c, &n, r);
        if (n) {
            int i = 0;
            while (i < g_pr.nsc && memcmp(&g_pr.sc[i], r, sizeof *r)) i++;
            if (i == g_pr.nsc && g_pr.nsc < 8) g_pr.sc[g_pr.nsc++] = r[0];
        }
    }
}
// The game's batch flush starts (from the flush stub): the batch is noted, and while the probe's frame draws the panel
// the game's way, the records about to be sent (count at +0x714E4, 0x74 bytes each from +0x64: the flush's own copy)
// are scanned: each of their 29 dwords' lowest and highest value as a float, and the first two records whole.
#define PR_REC 0x74
#define PR_FIELDS (PR_REC / 4)
static int g_pr_rec;               // 1 while the panel is drawn the game's way in the probe's frame
static unsigned g_pr_nrec, g_pr_nflush;
static float g_pr_lo[PR_FIELDS], g_pr_hi[PR_FIELDS];
static DWORD g_pr_first[2][PR_FIELDS];
static void __cdecl pr_flush(void *batch) {
    if (!g_pr_rec) return;
    unsigned n = *(const unsigned *)((const char *)batch + 0x714E4);
    const float *r = (const float *)((const char *)batch + 0x64);
    if (n > 4000) n = 4000;
    g_pr_nflush++;
    for (unsigned i = 0; i < n; i++, r += PR_FIELDS) {
        if (g_pr_nrec < 2) memcpy(g_pr_first[g_pr_nrec], r, PR_REC);
        for (int k = 0; k < PR_FIELDS; k++) {
            if (!g_pr_nrec || r[k] < g_pr_lo[k]) g_pr_lo[k] = r[k];
            if (!g_pr_nrec || r[k] > g_pr_hi[k]) g_pr_hi[k] = r[k];
        }
        g_pr_nrec++;
    }
}
static void pr_flush_report(void) {
    if (!g_log) return;
    g_log("probe: the panel drawn the game's way: %u records in %u flushes", g_pr_nrec, g_pr_nflush);
    for (int k = 0; k < PR_FIELDS; k++)
        g_log("probe:   field +0x%02X: lowest %g, highest %g; record 1 %08lx (%g), record 2 %08lx (%g)", k * 4, g_pr_lo[k], g_pr_hi[k], g_pr_first[0][k],
              ((float *)g_pr_first[0])[k], g_pr_first[1][k], ((float *)g_pr_first[1])[k]);
}

static void pr_redraw_done(void) {
    char vp[8 * 48] = "", sc[8 * 40] = "", rt[8 * 24] = "";
    for (int i = 0; i < g_pr.nvp; i++)
        snprintf(vp + strlen(vp), sizeof vp - strlen(vp), " (%.0f,%.0f %.0fx%.0f)", g_pr.vp[i].TopLeftX, g_pr.vp[i].TopLeftY, g_pr.vp[i].Width, g_pr.vp[i].Height);
    for (int i = 0; i < g_pr.nsc; i++)
        snprintf(sc + strlen(sc), sizeof sc - strlen(sc), " (%ld,%ld-%ld,%ld)", g_pr.sc[i].left, g_pr.sc[i].top, g_pr.sc[i].right, g_pr.sc[i].bottom);
    for (int i = 0; i < g_pr.nrt; i++) snprintf(rt + strlen(rt), sizeof rt - strlen(rt), " %p%s", g_pr.rt[i], g_pr.rt[i] == (void *)g_bb ? "(its target)" : "");
    if (g_log)
        g_log("probe: redraw %d: draws into the textures %u, elsewhere %u; targets set %u:%s; viewports set %u, distinct:%s; scissor on in %u draws, off in %u, rects:%s",
              g_pr_redraws + 1, g_pr.in, g_pr.out, g_pr.rts, rt, g_pr.vps, vp, g_pr.sc_on, g_pr.sc_off, sc);
    memset(&g_pr, 0, sizeof g_pr);
    g_pr_redraws++;
}
#endif

// The game's calls while a panel is drawn into its textures, before d3d11 gets them: sp[1] is the context, sp[2] on
// the arguments, which may be changed.
static __attribute__((noinline)) void uic_event(int kind, void **sp) {
    Target *t = g_red;
    ID3D11DeviceContext *c = sp[1];
#ifdef PROFILING
    if (g_pr_state == 1) pr_event(kind, sp);
#endif
    if (kind == F_RT) {          // OMSetRenderTargets(count, views, depth): the old target stands for the black texture
        static ID3D11RenderTargetView *mine[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
        ID3D11RenderTargetView **v = sp[3];
        unsigned n = (unsigned)(uintptr_t)sp[2];
        g_game_rt = n && v ? v[0] : NULL;
        int is = g_game_rt == g_bb && n <= D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;
        if (is) {
            memcpy(mine, v, n * sizeof *v);
            mine[0] = t->rtv[0];
            sp[3] = mine;
            sp[4] = NULL;   // no depth buffer: ours are another size (the UI does not test depth)
        } else if (g_game_rt == g_bb) g_red_bad = 1;
    } else if (kind == F_VP) {   // RSSetViewports(count, viewports): noted, the records' positions are in it
        const D3D11_VIEWPORT *v = sp[3];
        if ((unsigned)(uintptr_t)sp[2] && v) g_game_vp = v[0];
    } else if (g_game_rt == g_bb) {   // a draw into the panel: once more, into the white texture
        const unsigned *a = (const unsigned *)(sp + 2);
        ID3D11BlendState *bs = NULL;
        FLOAT bf[4];
        UINT mask;
        ID3D11DeviceContext_OMGetBlendState(c, &bs, bf, &mask);
        if (!blend_ok(bs)) g_red_bad = 1;
        REL(bs);
        g_own = 1;
        g_set_rt(c, 1, &t->rtv[1], NULL);
        if (kind == F_DRAW) ID3D11DeviceContext_Draw(c, a[0], a[1]);
        else if (kind == F_DRAWI) ID3D11DeviceContext_DrawIndexed(c, a[0], a[1], (INT)a[2]);
        else if (kind == F_DRAWN) ID3D11DeviceContext_DrawInstanced(c, a[0], a[1], a[2], a[3]);
        else ID3D11DeviceContext_DrawIndexedInstanced(c, a[0], a[1], a[2], (INT)a[3], a[4]);
        g_set_rt(c, 1, &t->rtv[0], NULL);
        g_own = 0;
    }
}

// Every call into a patched function while a panel is redrawn comes through here first (the stub saves all registers
// and the flags around it). No floating point or SSE in here: d3d11's own calls between its two functions per call
// come through too, and are passed on untouched, as are ours and another context's.
__attribute__((target("general-regs-only"))) static void __cdecl uic_d3d(unsigned i, void **sp) {
    if ((uintptr_t)sp[0] - g_d3d_lo < g_d3d_size || g_own || !g_red || sp[1] != (void *)g_ctx) return;
    uic_event(g_fkind[i], sp);
}

// One of d3d11's functions gets our jump at its start ("mov edi, edi" with 5 bytes of padding before it; behind another
// mod's hot patch or 5-byte jump ours goes in front). 1: it is patched.
static int patch_fn(void *fn, int kind) {
    HMODULE d3d = GetModuleHandleW(L"d3d11.dll"), owner = NULL;
    for (int i = 0; i < g_nfn; i++) if (g_fn[i] == fn) return 1;
    if (!fn || !d3d || g_nfn == NFN) return 0;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)fn, &owner);
    if (owner != d3d) return 0;   // another mod's function in the vtable slot: what it calls is not ours to guess
    if (!g_d3d_size) {
        const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((unsigned char *)d3d + ((const IMAGE_DOS_HEADER *)d3d)->e_lfanew);
        g_d3d_lo = (uintptr_t)d3d;
        g_d3d_size = nt->OptionalHeader.SizeOfImage;
    }
    unsigned char *f = fn, *next = f + 2, *where = f - 5;
    int mode = f[0] == 0xEB && f[1] == 0xF9 && f[-5] == 0xE9 ? 1 : f[0] == 0xE9 ? 2 : 0;
    if (mode == 1) next = f + *(int *)(f - 4);
    else if (mode == 2) { next = f + 5 + *(int *)(f + 1); where = f; }
    int ok = mode == 2 ? ((uintptr_t)f & 63) <= 59 : !((uintptr_t)f & 3);   // the bytes rewritten below: one aligned write, or in one cache line
    if (!mode) {
        ok &= f[0] == 0x8B && f[1] == 0xFF;
        for (int i = -5; i < 0; i++) ok &= f[i] == 0xCC || f[i] == 0x90;
    }
    DWORD prot;
    if (!ok || (!g_thunks && !(g_thunks = VirtualAlloc(NULL, NFN * 48, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))) ||
        !VirtualProtect(where, 7, PAGE_EXECUTE_READWRITE, &prot))
        return 0;
    unsigned char *t = g_thunks + 48 * g_nfn;
    static const unsigned char stub[27] = {0x9C, 0x60,                    // 08 pushfd; pushad
                                           0x8D, 0x44, 0x24, 0x24, 0x50,  // 0A lea eax, [esp+36]; push eax   (the return address)
                                           0x68, 0, 0, 0, 0,              // 0F push index
                                           0xE8, 0, 0, 0, 0,              // 14 call uic_d3d
                                           0x83, 0xC4, 0x08, 0x61, 0x9D,  // 19 add esp, 8; popad; popfd
                                           0xE9, 0, 0, 0, 0};             // 1E jmp next
    t[0] = 0xFF; t[1] = 0x25; *(void **)(t + 2) = &g_uic_go[g_nfn];     // 00 jmp [g_uic_go[i]]
    memcpy(t + 8, stub, sizeof stub);
    *(int *)(t + 0x10) = g_nfn;
    *(int *)(t + 0x15) = (int)((unsigned char *)(void *)uic_d3d - (t + 0x19));
    *(int *)(t + 0x1F) = (int)(next - (t + 0x23));
    g_fn[g_nfn] = fn; g_next[g_nfn] = next; g_uic_go[g_nfn] = next; g_fkind[g_nfn] = (unsigned char)kind;
    if (mode == 1) *(volatile int *)(f - 4) = (int)(t - f);            // one 4-byte write: the jump is whole at all times
    else if (mode == 2) *(volatile int *)(f + 1) = (int)(t - (f + 5));
    else {
        f[-5] = 0xE9;
        *(int *)(f - 4) = (int)(t - f);
        *(volatile unsigned short *)f = 0xF9EB;                        // jmp short -7 (one 2-byte write)
    }
    VirtualProtect(where, 7, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), where, 7);
    if (g_log) g_log("battle UI cache: d3d11+%#lx hot-patched%s", (unsigned long)(f - (unsigned char *)d3d), mode ? " (in front of another mod's hook)" : "");
    g_nfn++;
    return 1;
}

// The functions in the context's six slots are patched. d3d11 keeps two per call and swaps them (after a Flush the
// other one is in the slot), so the first time they are looked at before and after a Flush, and again at every redraw.
// 0: one cannot be patched - nothing can be cached.
static int patch_slots(ID3D11DeviceContext *c) {
    HMODULE d3d = GetModuleHandleW(L"d3d11.dll");
    int ok = 1;
    for (int k = 0; k < F_KINDS; k++) {
        void *fn = (*(void ***)c)[g_slotof[k]];
        HMODULE owner = NULL;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)fn, &owner);
        if (owner != d3d) fn = fgb_d3d11_original ? fgb_d3d11_original(g_slotof[k]) : NULL;   // behind the add-on's hook; NULL: another mod's function
        // the two instanced draws may stay unpatched: the UI does not use them
        ok &= patch_fn(fn, k) || k >= F_DRAWN;
        if (k == F_RT) g_set_rt = fn;
        if (k == F_VP) g_set_vp = fn;
    }
    return ok;
}
static int watch_slots(ID3D11DeviceContext *c) {
    static int flushed;
    int ok = patch_slots(c);
    if (!flushed) {
        flushed = 1;
        ID3D11DeviceContext_Flush(c);
        ok &= patch_slots(c);
    }
    if (!ok && !g_patch_failed++ && g_log) g_log("battle UI cache: one of d3d11's functions has no patch point - nothing is cached");
    return ok;
}

// The panel's two textures: at least w x h, grown in steps of 64 and never shrunk. Half floats whatever the target
// is: an additive draw must not clip in the white one (an 8-bit pair was up to 26 of 255 off in Attila's test).
static int make_textures(Target *t, unsigned w, unsigned h) {
    if (t->tex[0] && t->tw >= w && t->th >= h) return 1;
    w = (w + 63) & ~63u; h = (h + 63) & ~63u;
    if (w < t->tw) w = t->tw;
    if (h < t->th) h = t->th;
    t->valid = 0;
    D3D11_TEXTURE2D_DESC d = {w, h, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
    int ok = 1;
    for (int i = 0; i < 2; i++) {
        REL(t->srv[i]); REL(t->rtv[i]); REL(t->tex[i]);
        ok = ok && SUCCEEDED(ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &t->tex[i])) &&
             SUCCEEDED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)t->tex[i], NULL, &t->rtv[i])) &&
             SUCCEEDED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)t->tex[i], NULL, &t->srv[i]));
    }
    if (!ok) {
        for (int i = 0; i < 2; i++) { REL(t->srv[i]); REL(t->rtv[i]); REL(t->tex[i]); }
        t->tw = t->th = 0;
        return 0;
    }
    t->tw = w; t->th = h;
    if (g_log) g_log("battle UI cache: %s gets two %ux%u half-float textures (%u MB)", t->name, w, h, (w * h + 32768) / 65536);
    return 1;
}

// From here the game's draws go into the panel's textures. 0: not possible now, it is drawn the game's way.
static int redraw_begin(Target *t, ID3D11DeviceContext *c) {
    static const FLOAT black[4] = {0, 0, 0, 0}, white[4] = {1, 1, 1, 1};
    ID3D11Resource *res = NULL;
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    D3D11_TEXTURE2D_DESC td = {0};
    UINT nvp = 1;
    if (!watch_slots(c)) return 0;
    g_bb = NULL; g_dsv = NULL;
    ID3D11DeviceContext_OMGetRenderTargets(c, 1, &g_bb, &g_dsv);
    if (g_bb) ID3D11RenderTargetView_GetResource(g_bb, &res);
    if (res) ID3D11Resource_GetType(res, &dim);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &td);
    REL(res);
    ID3D11DeviceContext_RSGetViewports(c, &nvp, &g_game_vp);
    if (!td.Width || !td.Height || td.SampleDesc.Count != 1 || !nvp || !make_textures(t, td.Width, td.Height)) {
        REL(g_bb); REL(g_dsv);
        return 0;
    }
    t->tgt_w = td.Width; t->tgt_h = td.Height;
    g_brec = 0;
    g_red_bad = 0;
    ID3D11DeviceContext_ClearRenderTargetView(c, t->rtv[0], black);
    ID3D11DeviceContext_ClearRenderTargetView(c, t->rtv[1], white);
    g_set_rt(c, 1, &t->rtv[0], NULL);
    g_game_rt = g_bb;
    g_red = t;
    for (int i = 0; i < g_nfn; i++) g_uic_go[i] = g_thunks + 48 * i + 8;
    return 1;
}

// The game has drawn the panel: the device gets its target and its viewport again.
static void redraw_end(ID3D11DeviceContext *c) {
    Target *t = g_red;
#ifdef PROFILING
    if (g_pr_state == 1) pr_redraw_done();
#endif
    for (int i = 0; i < g_nfn; i++) g_uic_go[i] = g_next[i];
    if (g_game_rt == g_bb) ID3D11DeviceContext_OMSetRenderTargets(c, 1, &g_bb, g_dsv);
    else g_red_bad = 1;   // it left another target bound: that one stays, and this panel is not cached again
    g_red = NULL;
    REL(g_bb); REL(g_dsv);
    // the box: whole pixels around the records' extent and 2 more (a quad's edge pixel, filtering), cut to the target
    int b[4] = {0, 0, 0, 0};
    if (g_brec) {
        float f[4] = {g_bx0 - 2, g_by0 - 2, g_bx1 + 3, g_by1 + 3}, lim[4] = {0, 0, (float)t->tgt_w, (float)t->tgt_h};
        for (int i = 0; i < 4; i++) b[i] = f[i] < 0 ? 0 : f[i] > lim[i & 1 ? 3 : 2] ? (int)lim[i & 1 ? 3 : 2] : (int)f[i];
    }
    memcpy(t->box, b, sizeof b);
    if (!t->said && g_log) {   // what the cache measured, for a bug report
        t->said = 1;
        g_log("battle UI cache: %s drawn at %d,%d to %d,%d on the %u x %u target (%u draw records; its layout fields %.0f,%.0f %.0f x %.0f)", t->name, b[0], b[1], b[2],
              b[3], t->tgt_w, t->tgt_h, g_brec, t->rect[0], t->rect[1], t->rect[2], t->rect[3]);
    }
    t->valid = !g_red_bad;
    if (g_red_bad) {
        t->off = 1;   // what is in the textures is shown this once
        if (g_log) g_log("battle UI cache: %s is drawn the game's way from now on (blending or a render target the cache cannot stand in for)", t->name);
    }
}

// The textures over the scene. Everything set for it is put back: the game keeps its own record of what is bound.
static void composite(Target *t, ID3D11DeviceContext *c) {
    ID3D11VertexShader *vs = NULL; ID3D11PixelShader *ps = NULL; ID3D11GeometryShader *gs = NULL;
    ID3D11InputLayout *il = NULL; ID3D11ShaderResourceView *srv[2] = {NULL, NULL}; ID3D11BlendState *bs = NULL;
    ID3D11DepthStencilState *dss = NULL; ID3D11RasterizerState *rs = NULL;
    D3D11_PRIMITIVE_TOPOLOGY topo; FLOAT bf[4]; UINT mask, sref, nvp = 1;
    D3D11_VIEWPORT vp = {0}, mine = {(FLOAT)t->box[0], (FLOAT)t->box[1], (FLOAT)(t->box[2] - t->box[0]), (FLOAT)(t->box[3] - t->box[1]), 0, 1};
    if (t->box[2] <= t->box[0] || t->box[3] <= t->box[1]) return;   // it drew nothing on the target
    ID3D11DeviceContext_VSGetShader(c, &vs, NULL, NULL);
    ID3D11DeviceContext_PSGetShader(c, &ps, NULL, NULL);
    ID3D11DeviceContext_GSGetShader(c, &gs, NULL, NULL);
    ID3D11DeviceContext_IAGetInputLayout(c, &il);
    ID3D11DeviceContext_IAGetPrimitiveTopology(c, &topo);
    ID3D11DeviceContext_PSGetShaderResources(c, 0, 2, srv);
    ID3D11DeviceContext_OMGetBlendState(c, &bs, bf, &mask);
    ID3D11DeviceContext_OMGetDepthStencilState(c, &dss, &sref);
    ID3D11DeviceContext_RSGetState(c, &rs);
    ID3D11DeviceContext_RSGetViewports(c, &nvp, &vp);

    static const FLOAT one[4] = {1, 1, 1, 1};
    ID3D11DeviceContext_IASetInputLayout(c, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(c, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(c, g_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(c, g_ps, NULL, 0);
    if (gs) ID3D11DeviceContext_GSSetShader(c, NULL, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(c, 0, 2, t->srv);
    ID3D11DeviceContext_OMSetBlendState(c, g_blend, one, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(c, g_dss, 0);
    ID3D11DeviceContext_RSSetState(c, g_rs);
    g_set_vp(c, 1, &mine);
    ID3D11DeviceContext_Draw(c, 3, 0);

    ID3D11DeviceContext_IASetInputLayout(c, il);
    ID3D11DeviceContext_IASetPrimitiveTopology(c, topo);
    ID3D11DeviceContext_VSSetShader(c, vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(c, ps, NULL, 0);
    if (gs) ID3D11DeviceContext_GSSetShader(c, gs, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(c, 0, 2, srv);
    ID3D11DeviceContext_OMSetBlendState(c, bs, bf, mask);
    ID3D11DeviceContext_OMSetDepthStencilState(c, dss, sref);
    ID3D11DeviceContext_RSSetState(c, rs);
    if (nvp) ID3D11DeviceContext_RSSetViewports(c, 1, &vp);
    REL(vs); REL(ps); REL(gs); REL(il); REL(srv[0]); REL(srv[1]); REL(bs); REL(dss); REL(rs);
}

// Once a frame: 1 for 250 ms after a mouse button or a key that can change the cards goes down or comes up. Held, it
// does not count (Attila run 5: 620 of 877 frames in 5 s were redrawn for input with a button held to turn the camera;
// a redrawn frame costs more than no cache). The camera's keys (W A S D Q E Z X, the arrows, the number pad) and the
// modifiers alone do not count at all, nor does the mouse moving or its wheel. Looked at only when Windows has seen
// input: a key cannot go down or up without.
static POINT g_cursor;
static SHORT(WINAPI *g_key)(int) = GetAsyncKeyState;
static int g_test_keys;            // the test has keys of its own (g_key), read every frame
static int input_live(DWORD now) {
    static const unsigned char keys[] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2, VK_BACK, VK_TAB, VK_RETURN, VK_ESCAPE, VK_SPACE,
                                         VK_PRIOR, VK_NEXT, VK_END, VK_HOME, VK_INSERT, VK_DELETE, '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
                                         'B', 'C', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'R', 'T', 'U', 'V', 'Y',
                                         VK_F1, VK_F2, VK_F3, VK_F4, VK_F5, VK_F6, VK_F7, VK_F8, VK_F9, VK_F10, VK_F11, VK_F12};
    static LONG frame = -1;
    static DWORD last, until;
    static unsigned char was[sizeof keys];
    if (!g_base && !g_test_keys) return 0;
    if (frame != g_frame_no) {
        frame = g_frame_no;
        LASTINPUTINFO li = {sizeof li, 0};
        GetCursorPos(&g_cursor);
        GetLastInputInfo(&li);
        if (li.dwTime != last || g_test_keys)
            for (unsigned i = 0; i < sizeof keys; i++) {
                unsigned char down = (g_key(keys[i]) & 0x8000) != 0;
                if (down != was[i]) { was[i] = down; until = now + 250; }
            }
        last = li.dwTime;
    }
    return (LONG)(until - now) > 0;
}

// The cursor over the panel's box (the window taken as the whole target), 16 window pixels around.
static int cursor_over(const Target *t) {
    if (!g_base || t->box[2] <= t->box[0] || !t->tgt_w || !t->tgt_h) return 0;
    RECT r;
    HWND w = GetForegroundWindow();
    POINT p = g_cursor;
    if (!w || !GetClientRect(w, &r) || r.right <= 0 || r.bottom <= 0 || !ScreenToClient(w, &p)) return 0;
    float x = (float)p.x * (float)t->tgt_w / (float)r.right, y = (float)p.y * (float)t->tgt_h / (float)r.bottom;
    float m = 16.0f * (float)t->tgt_w / (float)r.right;
    return x >= t->box[0] - m && y >= t->box[1] - m && x <= t->box[2] + m && y <= t->box[3] + m;
}

// Once a frame. Profiling build: a line to the log every 5 s.
static void frame_tick(void) {
    static LONG frame = -1;
    if (frame == g_frame_no) return;
    frame = g_frame_no;
    g_uic_batch = NULL;   // the UI's batch is known again once it has been flushed this frame
#ifdef PROFILING
    static DWORD said;
    DWORD now = GetTickCount();
    if (now - said < 5000) return;
    said = now;
    for (int i = 0; i < g_nt; i++) {
        Target *t = &g_t[i];
        if (g_log && (t->reuse || t->why[1] || t->why[2] || t->why[3] || t->why[4] || t->why[5]))
            g_log("battle UI cache: %s in 5 s: shown from the textures %u frames; redrawn %u (first %u, moved %u, key or button %u, cursor over it %u, interval %u)%s", t->name,
                  t->reuse, t->why[1] + t->why[2] + t->why[3] + t->why[4] + t->why[5], t->why[1], t->why[2], t->why[3], t->why[5], t->why[4], t->off ? " - OFF for it" : "");
        memset(t->why, 0, sizeof t->why);
        t->reuse = 0;
    }
    if (g_log && g_nobatch) g_log("battle UI cache: %u times in 5 s the game's batch was not known at a panel's edge (nothing flushed there)", g_nobatch);
    g_nobatch = 0;
#endif
}

// 1 if the component has an ancestor of that name (12 levels up at most).
static int is_under(void *self, const char *name) {
    for (int i = 0; i < 12 && (self = *(void **)((char *)self + C_PARENT)) != NULL; i++) {
        const char *n = *(const char **)((char *)self + C_NAME);
        if (n && !strcmp(n, name)) return 1;
    }
    return 0;
}

// The game's batch flush starts (the stub at its start calls this with the batch): the batch is noted, and while a
// panel is redrawn onto its target the box grows by the records about to be sent. The batch holds count at +0x714E4
// records of 0x74 bytes from +0x64, which the flush copies into the vertex buffer as they are (+0x1457FC0 Steam). A
// record starts with its position in clip space, x and y from -1 to 1 in the viewport (probe battle 2, 2026-11-25: the
// unit cards' 7,060 records spanned 794..3046 x 1869.. on the 4K target, the game's own frame 794..3045 x 1869..).
#define REC_SIZE 0x74
static void __cdecl uic_flush(void *batch) {
    g_uic_batch = batch;
#ifdef PROFILING
    pr_flush(batch);
#endif
    if (!g_red || g_game_rt != g_bb) return;
    unsigned n = *(const unsigned *)((const char *)batch + 0x714E4);
    const unsigned char *r = (const unsigned char *)batch + 0x64;
    const D3D11_VIEWPORT *v = &g_game_vp;
    if (n > 4000) n = 4000;   // the batch's size
    for (unsigned i = 0; i < n; i++, r += REC_SIZE) {
        float x = ((const float *)r)[0], y = ((const float *)r)[1];
        if (!(x >= -4 && x <= 4 && y >= -4 && y <= 4)) continue;   // not a position (NaN, garbage): left out
        x = v->TopLeftX + (x + 1) * 0.5f * v->Width;
        y = v->TopLeftY + (1 - y) * 0.5f * v->Height;
        if (!g_brec++) { g_bx0 = g_bx1 = x; g_by0 = g_by1 = y; continue; }
        if (x < g_bx0) g_bx0 = x;
        if (x > g_bx1) g_bx1 = x;
        if (y < g_by0) g_by0 = y;
        if (y > g_by1) g_by1 = y;
    }
}

// Before the game's Draw of a component (from the entry stub; sp[0] is its return address, sp[1] the DrawDetails).
// 0: the game's Draw runs, untouched - or, for a panel to be redrawn, with its return address swapped for the stub
// that calls uic_post. 1: the panel was shown from its textures, Draw is not run.
static int __cdecl uic_pre(void *self, void **sp) {
    if (g_red) return 0;   // inside a panel that is being drawn into its textures
    frame_tick();
#ifdef PROFILING
    if (!*(void **)((char *)self + C_PARENT)) memcpy(g_pr_root, sp[1], DETAILS);
#endif
    const char *name = *(const char **)((char *)self + C_NAME);
    Target *t = NULL;
    if (name)
        for (int i = 0; i < g_nt && !t; i++) {
            Target *x = &g_t[i];
            // a name is looked for only while its component has not been drawn for a while (it may be gone)
            if (x->comp == self ? !strcmp(name, x->name)
                                : (!x->comp || g_frame_no - x->seen > 30) && !strcmp(name, x->name) && (!x->under || is_under(self, x->under)))
                t = x;
        }
    if (!t) return 0;
    if (t->comp != self && g_log) g_log("battle UI cache: %s is component %p", t->name, self);
    t->comp = self;
    t->seen = g_frame_no;
    ID3D11DeviceContext *c = g_on && !t->off && *((unsigned char *)self + C_VISIBLE) ? device() : NULL;
    if (!c) { t->valid = 0; return 0; }
#ifdef PROFILING
    if (g_pr_state == 3) {   // this frame the game's way, the target saved after its Draw (uic_post)
        flush_pending();
        g_pr_state = 4;
        g_pr_after = 1;
        g_pr_rec = 1;   // from here to the flush after its Draw: the panel's records
        const float *cf = (const float *)self, *df = (const float *)sp[1], *sf = *(const float **)((char *)self + C_STATE);
        if (g_log)
            g_log("probe: that frame: %s relative +0x8C %g,%g, +0x94 %g,%g, state %g x %g; DrawDetails +0x8 %g,%g, +0x14 %g", t->name, cf[0x8C / 4], cf[0x90 / 4],
                  cf[0x94 / 4], cf[0x98 / 4], sf ? sf[0x28 / 4] : 0, sf ? sf[0x2C / 4] : 0, df[2], df[3], df[5]);
        g_uic_ret = sp[0];
        sp[0] = g_stubs + 0x28;
        return 0;
    }
#endif
    const float *pos = (const float *)((char *)self + C_POS), *st = *(const float **)((char *)self + C_STATE);
    float rect[4] = {pos[0], pos[1], st ? st[0x28 / 4] : 0, st ? st[0x2C / 4] : 0};
    DWORD now = GetTickCount();
    int live = input_live(now);
    int why = !t->valid ? 1 : memcmp(sp[1], t->details, DETAILS) || memcmp(rect, t->rect, sizeof rect) ? 2 : live ? 3 : cursor_over(t) ? 5 :
              now - t->drawn >= INTERVAL ? 4 : 0;
    flush_pending();   // what was gathered before the panel is drawn now, where it belongs
    if (!why) {
        t->reuse++;
        composite(t, c);
        return 1;
    }
    if (!redraw_begin(t, c)) { t->valid = 0; return 0; }
#ifdef PROFILING
    if (g_pr_state == 0) {
        g_pr_state = 1;
        pr_details("the root's", g_pr_root);
        pr_details(t->name, sp[1]);
        if (g_log) g_log("probe: the game's viewport at %s's Draw: %.0f,%.0f %.0f x %.0f", t->name, g_game_vp.TopLeftX, g_game_vp.TopLeftY, g_game_vp.Width, g_game_vp.Height);
    }
#endif
    memcpy(t->details, sp[1], DETAILS);
    memcpy(t->rect, rect, sizeof rect);
    t->drawn = now;
    t->why[why]++;
    g_uic_ret = sp[0];
    sp[0] = g_stubs + 0x28;
    return 0;
}

// The game's Draw of a redrawn panel has returned (from the second stub).
static void __cdecl uic_post(void) {
    Target *t = g_red;
#ifdef PROFILING
    if (!t && g_pr_after) {   // the probe's frame drawn the game's way
        g_pr_after = 0;
        flush_pending();
        g_pr_rec = 0;
        pr_flush_report();
        pr_save_target(g_ctx, "uicache_probe_game.bmp");
        if (g_log) g_log("probe: done - the game folder has uicache_probe_black.bmp, _white.bmp, _cached.bmp and _game.bmp");
        return;
    }
#endif
    if (!t) return;
    flush_pending();   // the panel's last quads: into the textures
    redraw_end(g_ctx);
    composite(t, g_ctx);
#ifdef PROFILING
    if (g_pr_state == 1 && g_pr_redraws >= 3) {
        g_pr_state = 3;
        pr_save(g_ctx, (ID3D11Resource *)t->tex[0], "uicache_probe_black.bmp");
        pr_save(g_ctx, (ID3D11Resource *)t->tex[1], "uicache_probe_white.bmp");
        pr_save_target(g_ctx, "uicache_probe_cached.bmp");
        if (g_log) g_log("probe: the panel's box %d,%d to %d,%d, textures %ux%u", t->box[0], t->box[1], t->box[2], t->box[3], t->tw, t->th);
    }
#endif
}

// A 5-byte jump at f to `to` (and NOPs to n bytes). Written before the game draws any UI (install time).
static int put_jump(unsigned char *f, int n, const void *to) {
    DWORD old;
    if (!VirtualProtect(f, (SIZE_T)n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    LONG rel = (LONG)((const unsigned char *)to - (f + 5));
    memset(f + 5, 0x90, (size_t)(n - 5));
    memcpy(f + 1, &rel, 4);
    f[0] = 0xE9;
    VirtualProtect(f, (SIZE_T)n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), f, (SIZE_T)n);
    return 1;
}

// The two stubs, and the jump to the first over Draw's first instruction (mov eax, [abs]: 5 bytes, copied as it is in
// memory, so with its address already moved by the loader).
static int hook_at(unsigned char *f) {
    if (f[0] != 0xA1 || !(g_stubs = VirtualAlloc(NULL, 96, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))) return 0;
    static const unsigned char entry[0x24] = {
        0x60,                                  // 00 pushad
        0x8D, 0x44, 0x24, 0x20,                // 01 lea eax, [esp+0x20]   (where the return address is)
        0x50, 0x51,                            // 05 push eax; push ecx    (uic_pre(self, sp))
        0xE8, 0, 0, 0, 0,                      // 07 call uic_pre
        0x83, 0xC4, 0x08,                      // 0C add esp, 8
        0x85, 0xC0, 0x75, 0x0B,                // 0F test eax, eax; jnz 1E
        0x61,                                  // 13 popad                 (every register as the game set it)
        0, 0, 0, 0, 0,                         // 14 the instruction the jump replaced
        0xE9, 0, 0, 0, 0,                      // 19 jmp f + 5
        0x61, 0xB0, 0x01,                      // 1E popad; mov al, 1      (Draw's return value)
        0xC2, 0x10, 0x00};                     // 21 ret 0x10
    static const unsigned char after[0x0D] = {
        0x60,                                  // 28 pushad                (al: Draw's return value)
        0xE8, 0, 0, 0, 0,                      // 29 call uic_post
        0x61,                                  // 2E popad
        0xFF, 0x25, 0, 0, 0, 0};               // 2F jmp [g_uic_ret]
    unsigned char *s = g_stubs;
    memcpy(s, entry, sizeof entry);
    *(int *)(s + 0x08) = (int)((unsigned char *)(void *)uic_pre - (s + 0x0C));
    memcpy(s + 0x14, f, 5);
    *(int *)(s + 0x1A) = (int)(f + 5 - (s + 0x1E));
    memcpy(s + 0x28, after, sizeof after);
    *(int *)(s + 0x2A) = (int)((unsigned char *)(void *)uic_post - (s + 0x2E));
    *(void **)(s + 0x31) = &g_uic_ret;
    return put_jump(f, 5, s);
}

// A stub at the start of the game's flush hands the batch (ecx) to uic_flush, every register and the stack as they were.
static int hook_flush(unsigned char *f) {
    static const unsigned char head[6] = {0x83, 0xEC, 0x4C, 0x56, 0x8B, 0xF1};   // sub esp, 0x4C; push esi; mov esi, ecx
    if (memcmp(f, head, sizeof head)) return 0;
    unsigned char *s = g_stubs + 0x40;
    s[0] = 0x60; s[1] = 0x51;                                     // 40 pushad; push ecx
    s[2] = 0xE8; *(int *)(s + 3) = (int)((unsigned char *)(void *)uic_flush - (s + 7));   // 42 call uic_flush
    s[7] = 0x83; s[8] = 0xC4; s[9] = 0x04; s[10] = 0x61;          // 47 add esp, 4; popad
    memcpy(s + 11, head, sizeof head);                            // 4B the three instructions the jump replaces
    s[17] = 0xE9; *(int *)(s + 18) = (int)(f + 6 - (s + 22));     // 51 jmp f + 6 (ends at 0x56, the stubs have 96)
    if (!put_jump(f, 6, g_stubs + 0x40)) return 0;
    g_flush = (void(__fastcall *)(void *, void *))(void *)f;
    return 1;
}

// FNV-1a of n bytes of code as on disk, in pieces fnv1a_unreloc can take
static DWORD uc_hash(const unsigned char *b, DWORD rva, DWORD n) {
    DWORD h = 0;
    for (DWORD o = 0; o < n; o += 0x2F0) h = h * 0x01000193u ^ fnv1a_unreloc(b, rva + o, n - o < 0x2F0 ? n - o : 0x2F0);
    return h;
}

// the entry whose code matches m, or NULL
static const DWORD *uc_find(const unsigned char *b) {
    for (int i = 0; i < NBUILDS; i++) {
        const DWORD *e = g_uc_builds[i];
        if (!in_image(b, e[B_DRAW], e[B_DRAW_LEN]) || !in_image(b, e[B_FLUSH], LEN_FLUSH) || !in_image(b, e[B_VT], 0x74) ||
            !in_image(b, e[B_CREATE], e[B_CREATE_LEN]))
            continue;
        if (uc_hash(b, e[B_DRAW], e[B_DRAW_LEN]) == e[B_DRAW_FNV] && uc_hash(b, e[B_FLUSH], LEN_FLUSH) == e[B_FLUSH_FNV] &&
            uc_hash(b, e[B_CREATE], e[B_CREATE_LEN]) == e[B_CREATE_FNV] && *(const DWORD *)(b + e[B_VT] + 0x70) == (DWORD)(UINT_PTR)(b + e[B_CREATE]))
            return e;
    }
    return NULL;
}

static void add_target(const char *name, const char *under) {
    if (g_nt < NTARGET && *name) { strncat(g_t[g_nt].name, name, sizeof g_t[g_nt].name - 1); g_t[g_nt].under = under; g_nt++; }
}
// The battle's unit cards: cards_panel, under battle_orders, as Attila caches them. Rome 2's own layout (data.pack
// ui\battle ui\layout, read 2026-11-25) has Attila's tree: battle_orders > battle_orders_pane > card_panel_docker >
// cards_panel, the docker 660 x 192 and cards_panel 660 x 189 as laid out, cards_panel widened to fit the cards.
// 2.2 cached the docker (its name was found in the DLL, cards_panel is only in the layout): textures for the docker's
// box (768 x 320 in the game's log), and the unit cards were not shown in game. uicache_test case 9 has that tree: the
// docker cached, the cards drawn past its box are missing; cards_panel cached, the picture is the game's.
static void add_default_targets(void) { add_target("cards_panel", "battle_orders"); }

// m: the game module; NULL in tests, which pass a function of their own that starts with the same instruction and
// set the context, the sizes and the root themselves. names: one per line (NULL: the battle's unit cards).
// Needs g_frame_no to count frames: install only when the tile scan's frame hook is in.
int install_uicache(HMODULE m, const char *names, void *test_draw, void (*log)(const char *fmt, ...)) {
    static int done;
    if (done) return 0;
    done = 1;
    g_log = log;
    char buf[256] = "";
    strncat(buf, names ? names : "", sizeof buf - 1);
    for (char *p = strtok(buf, "\r\n\t ,"); p; p = strtok(NULL, "\r\n\t ,")) add_target(p, NULL);
    if (!g_nt) add_default_targets();
    if (!m) return hook_at(test_draw);

    unsigned char *base = (unsigned char *)m;
    const DWORD *e = uc_find(base);
    if (!e || !hook_at(base + e[B_DRAW]) || !hook_flush(base + e[B_FLUSH])) {
        if (log) log("battle UI cache: the game's UI drawing code differs from the known game versions - not installed");
        return 0;
    }
    g_build = e;
    g_renderer = *(unsigned char ***)(base + e[B_FLUSH] + 0x30);   // the flush's "mov ecx, [renderer]"
    g_base = base;
    if (log) log("battle UI cache: the unit cards (%s) are shown from a cache, redrawn about 15 times a second and on input", g_t[0].name);
    return 1;
}
