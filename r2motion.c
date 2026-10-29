// Per-object motion vectors for Rome 2 (dlss/DLSS_LOG.md 19-23). The animated models of the main view (the game's three
// skinned vertex shaders, recognised by the glue) are recorded at their draws and drawn again at the HUD-less point, after
// the bridge's camera-motion pass, with our shaders (r2motionvs.h: the game's skinning, bit-identical, this frame and the
// previous one) into the motion vector target, depth test EQUAL against the game's depth through the same (jittered /
// shrunk) viewport. Everything stays on the GPU:
// - records: each draw's instance records (88 bytes) are copied by the GPU into our buffer at draw time (the game's
//   dynamic instance buffers are rewritten during the frame);
// - history: a compute pass matches every record to the previous frame's nearest record of the same model type within
//   0.35 m (G1: 99.4 % found in battle; poses are shared rows of the animation texture, so the previous bones are the
//   previous frame's copy of the matched record's row); no match -> camera motion only (never "ghosting");
// - the animation texture is copied at the pass (the game rewrites it next frame); camera = the bridge's main camera.
// The pass runs inside a D3D11.1 device context state of its own (the game's state is untouched), and its own calls must
// not be seen by the glue's hooks: r2m_internal is set meanwhile.
#define COBJMACROS
#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>
#include "r2motionvs.h"

#define M_RECS 65536      // records per frame (G1 battle: ~5,200)
#define M_DRAWS 4096      // animated main-view draws per frame (G1: up to 276)
#define M_CELLS 65536     // hash grid cells
volatile int r2m_internal;   // the pass's own D3D11 calls are running (the glue's hooks pass them straight on)

static ID3D11Device *m_dev;
static ID3D11Device1 *m_dev1;
static ID3D11DeviceContext *m_ctx;
static ID3D11DeviceContext1 *m_ctx1;
static ID3DDeviceContextState *m_state;
static void (*m_log)(const char *fmt, ...);
static int m_on, m_enabled = 1, m_view;   // m_view: the debug view is on (only then the own-motion target is written and cleared)

static ID3D11RenderTargetView *m_mv_rtv, *m_own_rtv;
#ifdef PROFILING   // the debug view (dev builds): each model pixel's own motion (camera left out), w = 1
static ID3D11Texture2D *m_own;
static ID3D11ShaderResourceView *m_own_srv;
#endif
static ID3DBlob *m_vsb[2];
static ID3D11VertexShader *m_vs[2], *m_vs_own[2];   // own: the debug view's variants
static ID3D11PixelShader *m_ps, *m_ps_own;
static ID3D11ComputeShader *m_cs_build, *m_cs_match;
static ID3D11Buffer *m_rec[2];          // records, this frame / previous (raw + vertex buffer)
static ID3D11Buffer *m_cls[2];          // model type per record (u32: 1 + type; raw)
static ID3D11Buffer *m_prev;            // per record: the matched previous record (64 bytes: world rows, row, history flag)
static ID3D11Buffer *m_heads, *m_next;  // hash grid of the previous frame's records
static ID3D11UnorderedAccessView *m_prev_uav, *m_heads_uav, *m_next_uav;
static ID3D11ShaderResourceView *m_rec_srv[2], *m_cls_srv[2];
static ID3D11Buffer *m_cb_motion, *m_cb_cam[2], *m_cb_asz[2], *m_cb_cs;
static ID3D11Texture2D *m_anim[2];      // copies of the game's animation texture, this frame / previous
static ID3D11ShaderResourceView *m_anim_srv[2];
static D3D11_TEXTURE2D_DESC m_anim_desc;
static ID3D11SamplerState *m_point;
static ID3D11DepthStencilState *m_dss;
static ID3D11RasterizerState *m_rs;
static int m_cur;                       // which of the pairs is this frame's
static unsigned m_frame, m_prev_frame = ~0u, m_nprev;   // pass counter; the previous records' pass and count
static unsigned char m_cls_cpu[M_RECS];
static unsigned m_nrec;
typedef struct {
    ID3D11Buffer *vb, *ib; UINT stride, off, ioff; DXGI_FORMAT ifmt; int indexed, type; UINT count, inst, start; INT base;
    unsigned rec; ID3D11InputLayout *il; ID3D11Resource *anim;
} MDraw;
static MDraw m_draws[M_DRAWS];
static unsigned m_ndraws;
typedef struct { const void *game; int type; ID3D11InputLayout *ours; } MLayout;
static MLayout m_layouts[64];
static unsigned m_nlayouts;
static unsigned m_stat_draws, m_stat_skipped, m_stat_recs, m_stat_found, m_stat_passes, m_stat_tex;

// the matching: records are 88 bytes (world rows T0..T2 at 0 / 16 / 32, translation in .w; T3.x the animation row at 48)
static const char cs_src[] =
    "ByteAddressBuffer rec : register(t0);\n"        // the records being placed in the grid / matched
    "ByteAddressBuffer cls : register(t1);\n"
    "ByteAddressBuffer prev_rec : register(t2);\n"   // the previous frame's
    "ByteAddressBuffer prev_cls : register(t3);\n"
    "RWByteAddressBuffer heads : register(u0);\n"
    "RWByteAddressBuffer next : register(u1);\n"
    "RWByteAddressBuffer outp : register(u2);\n"
    "cbuffer c : register(b0) { uint n; uint nprev; uint pad0, pad1; };\n"
    "float3 pos(ByteAddressBuffer b, uint i) { return asfloat(uint3(b.Load(i * 88 + 12), b.Load(i * 88 + 28), b.Load(i * 88 + 44))); }\n"
    "int3 cell(float3 p) { return (int3)floor(p / 0.5); }\n"
    "uint hashc(int3 c) { return ((uint)c.x * 73856093u ^ (uint)c.y * 19349663u ^ (uint)c.z * 83492791u) & 65535u; }\n"
    "[numthreads(64, 1, 1)] void build(uint3 id : SV_DispatchThreadID) {\n"   // previous records into the grid (lists)
    "  if (id.x >= nprev) return;\n"
    "  uint old; heads.InterlockedExchange(hashc(cell(pos(prev_rec, id.x))) * 4, id.x + 1, old);\n"
    "  next.Store(id.x * 4, old); }\n"
    "[numthreads(64, 1, 1)] void match(uint3 id : SV_DispatchThreadID) {\n"
    "  if (id.x >= n) return;\n"
    "  float3 p = pos(rec, id.x); int3 c0 = cell(p); uint t = cls.Load(id.x * 4); uint best = 0; float bd = 0.35 * 0.35;\n"
    "  for (int dz = -1; dz <= 1; dz++) for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {\n"
    "    int3 cc = c0 + int3(dx, dy, dz); uint j = heads.Load(hashc(cc) * 4);\n"
    "    for (uint k = 0; j && k < 4096; k++) {\n"
    "      if (prev_cls.Load((j - 1) * 4) == t && all(cell(pos(prev_rec, j - 1)) == cc)) {\n"
    "        float3 d = pos(prev_rec, j - 1) - p; float dd = dot(d, d); if (dd < bd) { bd = dd; best = j; } }\n"
    "      j = next.Load((j - 1) * 4); } }\n"
    "  uint o = id.x * 64;\n"
    "  if (best) { uint b = (best - 1) * 88;\n"
    "    outp.Store4(o, prev_rec.Load4(b)); outp.Store4(o + 16, prev_rec.Load4(b + 16)); outp.Store4(o + 32, prev_rec.Load4(b + 32));\n"
    "    outp.Store4(o + 48, uint4(prev_rec.Load(b + 48), asuint(1.0), 0, 0)); }\n"
    "  else outp.Store4(o + 48, uint4(0, asuint(-1.0), 0, 0)); }\n";

static ID3D11Buffer *raw_buffer(UINT size, UINT bind) {
    D3D11_BUFFER_DESC d = {size, D3D11_USAGE_DEFAULT, bind, 0, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
    ID3D11Buffer *b = NULL;
    return SUCCEEDED(ID3D11Device_CreateBuffer(m_dev, &d, NULL, &b)) ? b : NULL;
}
static ID3D11ShaderResourceView *raw_srv(ID3D11Buffer *b, UINT size) {
    D3D11_SHADER_RESOURCE_VIEW_DESC d = {DXGI_FORMAT_R32_TYPELESS, D3D11_SRV_DIMENSION_BUFFEREX, {{0}}};
    d.BufferEx.FirstElement = 0; d.BufferEx.NumElements = size / 4; d.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    ID3D11ShaderResourceView *v = NULL;
    return SUCCEEDED(ID3D11Device_CreateShaderResourceView(m_dev, (ID3D11Resource *)b, &d, &v)) ? v : NULL;
}
static ID3D11UnorderedAccessView *raw_uav(ID3D11Buffer *b, UINT size) {
    D3D11_UNORDERED_ACCESS_VIEW_DESC d = {DXGI_FORMAT_R32_TYPELESS, D3D11_UAV_DIMENSION_BUFFER, {{0}}};
    d.Buffer.FirstElement = 0; d.Buffer.NumElements = size / 4; d.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    ID3D11UnorderedAccessView *v = NULL;
    return SUCCEEDED(ID3D11Device_CreateUnorderedAccessView(m_dev, (ID3D11Resource *)b, &d, &v)) ? v : NULL;
}
static ID3D11Buffer *cbuf(UINT size) {
    D3D11_BUFFER_DESC d = {(size + 15) & ~15u, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    ID3D11Buffer *b = NULL;
    return SUCCEEDED(ID3D11Device_CreateBuffer(m_dev, &d, NULL, &b)) ? b : NULL;
}

typedef HRESULT(WINAPI *comp_fn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, void *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);
int r2m_init(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *mv, void (*log)(const char *fmt, ...)) {
    static const GUID iid_dev1 = {0xa04bfb29, 0x08ef, 0x43d6, {0xa4, 0x9c, 0xa9, 0xbd, 0xbd, 0xcb, 0xe6, 0x86}};
    static const GUID iid_ctx1 = {0xbb2c6faa, 0xb5fb, 0x4082, {0x8e, 0x6b, 0x38, 0x8b, 0x8c, 0xfa, 0x90, 0xe1}};
    static const GUID iid_dev = {0xdb6f6ddb, 0xac77, 0x4e88, {0x82, 0x53, 0x81, 0x9d, 0xf9, 0xbb, 0xf1, 0x40}};   // ID3D11Device
    m_dev = dev; m_ctx = ctx; m_log = log;
    if (FAILED(ID3D11Device_QueryInterface(dev, &iid_dev1, (void **)&m_dev1)) || FAILED(ID3D11DeviceContext_QueryInterface(ctx, &iid_ctx1, (void **)&m_ctx1))) {
        log("motion pass: no D3D11.1 device - off");
        return 0;
    }
    D3D_FEATURE_LEVEL fl = ID3D11Device_GetFeatureLevel(dev);
    // Rome 2's device is single-threaded (flags 0x1 in its log): the state must say so or it is refused
    UINT sflags = ID3D11Device_GetCreationFlags(dev) & D3D11_CREATE_DEVICE_SINGLETHREADED ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
    HRESULT hr = ID3D11Device1_CreateDeviceContextState(m_dev1, sflags, &fl, 1, D3D11_SDK_VERSION, &iid_dev, NULL, &m_state);
    if (FAILED(hr)) { log("motion pass: no device context state of its own (0x%08lx) - off", hr); return 0; }
    comp_fn comp = (comp_fn)(void *)GetProcAddress(LoadLibraryExA("d3dcompiler_47.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32), "D3DCompile");
    D3D_SHADER_MACRO s4[2] = {{"SKIN4", "1"}, {NULL, NULL}};
    ID3DBlob *psb = NULL, *cb1 = NULL, *cb2 = NULL;
    int ok = comp && SUCCEEDED(comp(r2motion_hlsl, sizeof r2motion_hlsl - 1, "r2motion", NULL, NULL, "vs", "vs_5_0", 0, 0, &m_vsb[0], NULL)) &&
             SUCCEEDED(comp(r2motion_hlsl, sizeof r2motion_hlsl - 1, "r2motion", s4, NULL, "vs", "vs_5_0", 0, 0, &m_vsb[1], NULL)) &&
             SUCCEEDED(comp(r2motion_hlsl, sizeof r2motion_hlsl - 1, "r2motion", NULL, NULL, "ps", "ps_5_0", 0, 0, &psb, NULL)) &&
             SUCCEEDED(comp(cs_src, sizeof cs_src - 1, "r2match", NULL, NULL, "build", "cs_5_0", 0, 0, &cb1, NULL)) &&
             SUCCEEDED(comp(cs_src, sizeof cs_src - 1, "r2match", NULL, NULL, "match", "cs_5_0", 0, 0, &cb2, NULL));
    for (int k = 0; ok && k < 2; k++)
        ok = SUCCEEDED(ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(m_vsb[k]), ID3D10Blob_GetBufferSize(m_vsb[k]), NULL, &m_vs[k]));
    ok = ok && SUCCEEDED(ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &m_ps)) &&
         SUCCEEDED(ID3D11Device_CreateComputeShader(dev, ID3D10Blob_GetBufferPointer(cb1), ID3D10Blob_GetBufferSize(cb1), NULL, &m_cs_build)) &&
         SUCCEEDED(ID3D11Device_CreateComputeShader(dev, ID3D10Blob_GetBufferPointer(cb2), ID3D10Blob_GetBufferSize(cb2), NULL, &m_cs_match));
    if (psb) ID3D10Blob_Release(psb);
    if (cb1) ID3D10Blob_Release(cb1);
    if (cb2) ID3D10Blob_Release(cb2);
#ifdef PROFILING   // the debug view is in dev builds only
    // the debug view's variants (OWN: + the own-motion target); the same inputs, so the same input layouts
    D3D_SHADER_MACRO own[2][3] = {{{"OWN", "1"}, {NULL, NULL}}, {{"SKIN4", "1"}, {"OWN", "1"}, {NULL, NULL}}};
    for (int k = 0; ok && k < 3; k++) {
        ID3DBlob *b = NULL;
        ok = SUCCEEDED(comp(r2motion_hlsl, sizeof r2motion_hlsl - 1, "r2motion", own[k & 1], NULL, k < 2 ? "vs" : "ps", k < 2 ? "vs_5_0" : "ps_5_0", 0, 0, &b, NULL)) &&
             SUCCEEDED(k < 2 ? ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &m_vs_own[k])
                             : ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &m_ps_own));
        if (b) ID3D10Blob_Release(b);
    }
#endif
    for (int k = 0; ok && k < 2; k++) {
        ok = (m_rec[k] = raw_buffer(M_RECS * 88, D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_SHADER_RESOURCE)) &&
             (m_cls[k] = raw_buffer(M_RECS * 4, D3D11_BIND_SHADER_RESOURCE)) && (m_rec_srv[k] = raw_srv(m_rec[k], M_RECS * 88)) &&
             (m_cls_srv[k] = raw_srv(m_cls[k], M_RECS * 4)) && (m_cb_cam[k] = cbuf(208)) && (m_cb_asz[k] = cbuf(16));
    }
    ok = ok && (m_prev = raw_buffer(M_RECS * 64, D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_UNORDERED_ACCESS)) && (m_prev_uav = raw_uav(m_prev, M_RECS * 64)) &&
         (m_heads = raw_buffer(M_CELLS * 4, D3D11_BIND_UNORDERED_ACCESS)) && (m_heads_uav = raw_uav(m_heads, M_CELLS * 4)) &&
         (m_next = raw_buffer(M_RECS * 4, D3D11_BIND_UNORDERED_ACCESS)) && (m_next_uav = raw_uav(m_next, M_RECS * 4)) &&
         (m_cb_motion = cbuf(16)) && (m_cb_cs = cbuf(16)) && SUCCEEDED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)mv, NULL, &m_mv_rtv));
    D3D11_SAMPLER_DESC sd = {D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP,
                             0, 1, D3D11_COMPARISON_NEVER, {0, 0, 0, 0}, 0, D3D11_FLOAT32_MAX};
    D3D11_DEPTH_STENCIL_DESC dd = {TRUE, D3D11_DEPTH_WRITE_MASK_ZERO, D3D11_COMPARISON_EQUAL, FALSE, 0, 0, {1, 1, 1, 8}, {1, 1, 1, 8}};
    D3D11_RASTERIZER_DESC rd = {D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 0, 0, 0, TRUE, FALSE, FALSE, FALSE};
    ok = ok && SUCCEEDED(ID3D11Device_CreateSamplerState(dev, &sd, &m_point)) && SUCCEEDED(ID3D11Device_CreateDepthStencilState(dev, &dd, &m_dss)) &&
         SUCCEEDED(ID3D11Device_CreateRasterizerState(dev, &rd, &m_rs));
#ifdef PROFILING
    D3D11_TEXTURE2D_DESC od;   // the own-motion target: the motion texture's size (bound together)
    ID3D11Texture2D_GetDesc(mv, &od);
    od.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; od.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    od.Usage = D3D11_USAGE_DEFAULT; od.CPUAccessFlags = 0; od.MiscFlags = 0;
    ok = ok && SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &od, NULL, &m_own)) &&
         SUCCEEDED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)m_own, NULL, &m_own_rtv)) &&
         SUCCEEDED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)m_own, NULL, &m_own_srv));
#endif
    if (!ok) { log("motion pass: shaders / buffers not made - off"); return 0; }
    m_on = 1;
    log("motion pass ready: per-object motion of the animated models (GPU records, history matched on the GPU)");
    return 1;
}
void r2m_set_enabled(int on) { m_enabled = on; }
#ifdef PROFILING
void r2m_set_view(int on) { m_view = on; }   // (release builds: never on)
#endif
int r2m_ready(void) { return m_on; }

// our input layout for a game layout: its elements + PREV0..3 (slot 2), against our VS for the type (0: 2 bones, 1: 4)
static ID3D11InputLayout *layout_for(const void *game, const D3D11_INPUT_ELEMENT_DESC *e, UINT n, int type) {
    for (unsigned i = 0; i < m_nlayouts; i++) if (m_layouts[i].game == game && m_layouts[i].type == type) return m_layouts[i].ours;
    if (m_nlayouts == 64 || n > 16) return NULL;
    D3D11_INPUT_ELEMENT_DESC el[20];
    memcpy(el, e, n * sizeof *e);
    static const char prev[] = "PREV";
    for (UINT k = 0; k < 4; k++) {
        D3D11_INPUT_ELEMENT_DESC p = {prev, k, DXGI_FORMAT_R32G32B32A32_FLOAT, 2, 16 * k, D3D11_INPUT_PER_INSTANCE_DATA, 1};
        el[n + k] = p;
    }
    ID3D11InputLayout *il = NULL;
    if (FAILED(ID3D11Device_CreateInputLayout(m_dev, el, n + 4, ID3D10Blob_GetBufferPointer(m_vsb[type]), ID3D10Blob_GetBufferSize(m_vsb[type]), &il))) {
        m_log("motion pass: no input layout for the game's layout %p (type %d)", game, type);
        il = NULL;
    }
    MLayout l = {game, type, il};
    m_layouts[m_nlayouts++] = l;
    return il;
}

// at an animated main-view draw (glue: the game's context, scene depth bound, before the HUD-less point). type 0: 2 bones,
// 1: 4 bones. e / n: the game's input layout elements (semantic names valid), game_il its object.
void r2m_draw(ID3D11DeviceContext *c, int type, const void *game_il, const D3D11_INPUT_ELEMENT_DESC *e, UINT n, int indexed, UINT count,
              UINT inst, UINT start, INT base, UINT sinst) {
    if (!m_on || !m_enabled) return;
    m_stat_draws++;
    ID3D11InputLayout *il = layout_for(game_il, e, n, type);
    ID3D11Buffer *vb[2] = {0}, *ib = NULL;
    UINT str[2] = {0}, off[2] = {0}, ioff = 0;
    DXGI_FORMAT ifmt = DXGI_FORMAT_UNKNOWN;
    ID3D11ShaderResourceView *sr = NULL;
    ID3D11DeviceContext_IAGetVertexBuffers(c, 0, 2, vb, str, off);
    if (indexed) ID3D11DeviceContext_IAGetIndexBuffer(c, &ib, &ifmt, &ioff);
    ID3D11DeviceContext_VSGetShaderResources(c, 0, 1, &sr);
    ID3D11Resource *anim = NULL;
    if (sr) { ID3D11ShaderResourceView_GetResource(sr, &anim); ID3D11ShaderResourceView_Release(sr); }
    int ok = il && vb[0] && vb[1] && str[1] == 88 && anim && (!indexed || ib) && m_ndraws < M_DRAWS && m_nrec + inst <= M_RECS;
    if (ok && m_ndraws) ok = anim == m_draws[0].anim;   // shortcut: one animation texture per frame (G1); others skipped
    if (!ok) {
        m_stat_skipped++;
        for (int s = 0; s < 2; s++) if (vb[s]) ID3D11Buffer_Release(vb[s]);
        if (ib) ID3D11Buffer_Release(ib);
        if (anim) ID3D11Resource_Release(anim);
        return;
    }
    // this draw's records into ours (the GPU copy; the game rewrites its instance buffers during the frame)
    D3D11_BOX box = {off[1] + sinst * 88, 0, 0, off[1] + (sinst + inst) * 88, 1, 1};
    r2m_internal = 1;
    ID3D11DeviceContext_CopySubresourceRegion(c, (ID3D11Resource *)m_rec[m_cur], 0, m_nrec * 88, 0, 0, (ID3D11Resource *)vb[1], 0, &box);
    r2m_internal = 0;
    memset(m_cls_cpu + m_nrec, 1 + type, inst);
    MDraw d = {vb[0], ib, str[0], off[0], ioff, ifmt, indexed, type, count, inst, start, base, m_nrec, il, anim};
    m_draws[m_ndraws++] = d;   // keeps the references of vb[0], ib, anim
    m_nrec += inst;
    ID3D11Buffer_Release(vb[1]);
}

static void drop_draws(void) {
    for (unsigned i = 0; i < m_ndraws; i++) {
        ID3D11Buffer_Release(m_draws[i].vb);
        if (m_draws[i].ib) ID3D11Buffer_Release(m_draws[i].ib);
        ID3D11Resource_Release(m_draws[i].anim);
    }
    m_ndraws = 0;
    m_nrec = 0;
}

// at the HUD-less point (fgbridge.c's motion callback): match, draw, then this frame's data becomes the previous.
// dsv: a view of the scene depth; vp: this frame's camera block (560 bytes, view_projection at 144); rw / rh: render size;
// jx / jy: this frame's jitter (the viewport offset the scene was drawn with)
void r2m_pass(ID3D11DepthStencilView *dsv, const float *cam, UINT rw, UINT rh, float jx, float jy, float min_depth, float max_depth) {
    if (!m_on) return;
    m_frame++;
    if (!m_enabled || !m_ndraws || !dsv) { drop_draws(); m_prev_frame = ~0u; return; }
    r2m_internal = 1;
    ID3DDeviceContextState *game = NULL;
    ID3D11DeviceContext1_SwapDeviceContextState(m_ctx1, m_state, &game);
    ID3D11DeviceContext *c = m_ctx;
    // this frame's animation texture and camera / animation size blocks
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)m_draws[0].anim, &td);
    if (!m_anim[0] || td.Width != m_anim_desc.Width || td.Height != m_anim_desc.Height || td.Format != m_anim_desc.Format) {
        for (int k = 0; k < 2; k++) {
            if (m_anim_srv[k]) ID3D11ShaderResourceView_Release(m_anim_srv[k]);
            if (m_anim[k]) ID3D11Texture2D_Release(m_anim[k]);
            m_anim[k] = NULL; m_anim_srv[k] = NULL;
            D3D11_TEXTURE2D_DESC a = td;
            a.Usage = D3D11_USAGE_DEFAULT; a.BindFlags = D3D11_BIND_SHADER_RESOURCE; a.CPUAccessFlags = 0; a.MiscFlags = 0;
            if (SUCCEEDED(ID3D11Device_CreateTexture2D(m_dev, &a, NULL, &m_anim[k])))
                ID3D11Device_CreateShaderResourceView(m_dev, (ID3D11Resource *)m_anim[k], NULL, &m_anim_srv[k]);
        }
        m_anim_desc = td;
        m_prev_frame = ~0u;   // a new texture size: no usable history
        m_stat_tex++;
    }
    ID3D11DeviceContext_CopyResource(c, (ID3D11Resource *)m_anim[m_cur], m_draws[0].anim);
    float camb[52] = {0}, asz[4] = {(float)(td.Height - 1), 1.0f / td.Height, (float)(td.Width - 1), 1.0f / (td.Width - 1)};
    memcpy(camb + 36, cam + 36, 64);
    ID3D11DeviceContext_UpdateSubresource(c, (ID3D11Resource *)m_cb_cam[m_cur], 0, NULL, camb, 0, 0);
    ID3D11DeviceContext_UpdateSubresource(c, (ID3D11Resource *)m_cb_asz[m_cur], 0, NULL, asz, 0, 0);
    static unsigned cls32[M_RECS];   // each record's model type (1 + type) for the matching
    for (unsigned i = 0; i < m_nrec; i++) cls32[i] = m_cls_cpu[i];
    D3D11_BOX b4 = {0, 0, 0, m_nrec * 4, 1, 1};
    ID3D11DeviceContext_UpdateSubresource(c, (ID3D11Resource *)m_cls[m_cur], 0, &b4, cls32, 0, 0);
    // history: the previous frame's records into the grid, then each record matched (no previous pass: none)
    int has_prev = m_prev_frame == m_frame - 1;
    unsigned cs[4] = {m_nrec, has_prev ? m_nprev : 0, 0, 0};
    ID3D11DeviceContext_UpdateSubresource(c, (ID3D11Resource *)m_cb_cs, 0, NULL, cs, 0, 0);
    UINT zero[4] = {0, 0, 0, 0};
    ID3D11DeviceContext_ClearUnorderedAccessViewUint(c, m_heads_uav, zero);
    ID3D11ShaderResourceView *srv[4] = {m_rec_srv[m_cur], m_cls_srv[m_cur], m_rec_srv[!m_cur], m_cls_srv[!m_cur]};
    ID3D11UnorderedAccessView *uav[3] = {m_heads_uav, m_next_uav, m_prev_uav};
    ID3D11DeviceContext_CSSetShaderResources(c, 0, 4, srv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 3, uav, NULL);
    ID3D11DeviceContext_CSSetConstantBuffers(c, 0, 1, &m_cb_cs);
    if (cs[1]) {
        ID3D11DeviceContext_CSSetShader(c, m_cs_build, NULL, 0);
        ID3D11DeviceContext_Dispatch(c, (cs[1] + 63) / 64, 1, 1);
    }
    ID3D11DeviceContext_CSSetShader(c, m_cs_match, NULL, 0);
    ID3D11DeviceContext_Dispatch(c, (m_nrec + 63) / 64, 1, 1);
    ID3D11UnorderedAccessView *nuav[3] = {0};
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 3, nuav, NULL);
    // the draws: into the motion vectors where the game's depth is ours (EQUAL), through the scene's viewport
    float msz[4] = {(float)rw, (float)rh, 0, 0};
    ID3D11DeviceContext_UpdateSubresource(c, (ID3D11Resource *)m_cb_motion, 0, NULL, msz, 0, 0);
    ID3D11Buffer *cbs[5] = {m_cb_motion, m_cb_cam[m_cur], has_prev ? m_cb_cam[!m_cur] : m_cb_cam[m_cur], m_cb_asz[m_cur],
                            has_prev ? m_cb_asz[!m_cur] : m_cb_asz[m_cur]};
    ID3D11ShaderResourceView *ts[2] = {m_anim_srv[m_cur], has_prev ? m_anim_srv[!m_cur] : m_anim_srv[m_cur]};
    D3D11_VIEWPORT vp = {jx, jy, (float)rw, (float)rh, min_depth, max_depth};
    ID3D11RenderTargetView *rts[2] = {m_mv_rtv, m_own_rtv};
    ID3D11DeviceContext_OMSetRenderTargets(c, m_view ? 2 : 1, rts, dsv);   // the own-motion target only for the debug view
    ID3D11DeviceContext_OMSetDepthStencilState(c, m_dss, 0);
    ID3D11DeviceContext_RSSetState(c, m_rs);
    ID3D11DeviceContext_RSSetViewports(c, 1, &vp);
    ID3D11DeviceContext_VSSetConstantBuffers(c, 0, 5, cbs);
    ID3D11DeviceContext_PSSetConstantBuffers(c, 0, 1, cbs);
    ID3D11DeviceContext_VSSetShaderResources(c, 0, 2, ts);
    ID3D11DeviceContext_VSSetSamplers(c, 0, 1, &m_point);
    ID3D11DeviceContext_PSSetShader(c, m_view ? m_ps_own : m_ps, NULL, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(c, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for (unsigned i = 0; i < m_ndraws; i++) {
        MDraw *d = &m_draws[i];
        ID3D11Buffer *vbs[3] = {d->vb, m_rec[m_cur], m_prev};
        UINT st[3] = {d->stride, 88, 64}, of[3] = {d->off, 0, 0};
        ID3D11DeviceContext_IASetVertexBuffers(c, 0, 3, vbs, st, of);
        ID3D11DeviceContext_IASetInputLayout(c, d->il);
        ID3D11DeviceContext_VSSetShader(c, m_view ? m_vs_own[d->type] : m_vs[d->type], NULL, 0);
        if (d->indexed) {
            ID3D11DeviceContext_IASetIndexBuffer(c, d->ib, d->ifmt, d->ioff);
            ID3D11DeviceContext_DrawIndexedInstanced(c, d->count, d->inst, d->start, d->base, d->rec);
        } else ID3D11DeviceContext_DrawInstanced(c, d->count, d->inst, d->start, d->rec);
    }
    ID3D11DeviceContext1_SwapDeviceContextState(m_ctx1, game, NULL);
    if (game) ID3DDeviceContextState_Release(game);
    r2m_internal = 0;
    m_stat_passes++;
    m_stat_recs += m_nrec;
    // this frame's records, classes, texture and blocks are the previous ones next frame
    m_nprev = m_nrec;
    m_prev_frame = m_frame;
    m_cur = !m_cur;
    drop_draws();
}

// a frame that never got to the pass (no HUD-less point): its records are dropped and the next has no history
// (called after every Present: the debug view's texture cleared, so it only ever shows the models of a frame the pass ran in)
void r2m_frame_end(void) {
    if (m_ndraws) { drop_draws(); m_prev_frame = ~0u; }
    if (!m_on || !m_view) return;
    float none[4] = {0, 0, 0, 0};
    r2m_internal = 1;
    ID3D11DeviceContext_ClearRenderTargetView(m_ctx, m_own_rtv, none);
    r2m_internal = 0;
}

#ifdef PROFILING   // dev builds only
// the F11 panel's debug view: the models' own motion (the camera's left out, so zooming or turning leaves it alone) over the
// frame at Present, under the panel: moving model pixels tinted by direction, stronger the more they move; still ones grey
static const char show_src[] =
    "Texture2D<float4> own : register(t0);\n"
    "cbuffer v : register(b0) { float2 rsize; float2 bsize; };\n"
    "float4 vs(uint i : SV_VertexID) : SV_Position { float2 t = float2((i << 1) & 2, i & 2); return float4(t * float2(2, -2) + float2(-1, 1), 0, 1); }\n"
    "float4 ps(float4 p : SV_Position) : SV_Target {\n"
    "  float4 o = own.Load(int3(p.xy * rsize / bsize, 0)); float g = length(o.xy);\n"
    "  if (o.w == 0) discard;\n"
    "  if (g < 0.05) return float4(0.6, 0.6, 0.6, 0.35);\n"
    "  float2 d = o.xy / g;\n"
    "  return float4(saturate(float3(0.5 + 0.5 * d.x, 0.5 + 0.5 * d.y, 0.5 - 0.5 * d.x)), 0.45 + 0.45 * saturate(g / 2)); }\n";
static ID3D11VertexShader *m_show_vs;
static ID3D11PixelShader *m_show_ps;
static ID3D11BlendState *m_show_blend;
static ID3D11Buffer *m_cb_show;
static int m_show_bad;
void r2m_show(ID3D11Resource *bb, UINT rw, UINT rh) {
    if (!m_on || m_show_bad) return;
    if (!m_show_ps) {   // made once, on first use
        comp_fn comp = (comp_fn)(void *)GetProcAddress(LoadLibraryExA("d3dcompiler_47.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32), "D3DCompile");
        ID3DBlob *v = NULL, *p = NULL;
        D3D11_BLEND_DESC bd = {0};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA; bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA; bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO; bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE; bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        int ok = comp && SUCCEEDED(comp(show_src, sizeof show_src - 1, "r2show", NULL, NULL, "vs", "vs_5_0", 0, 0, &v, NULL)) &&
                 SUCCEEDED(comp(show_src, sizeof show_src - 1, "r2show", NULL, NULL, "ps", "ps_5_0", 0, 0, &p, NULL)) &&
                 SUCCEEDED(ID3D11Device_CreateVertexShader(m_dev, ID3D10Blob_GetBufferPointer(v), ID3D10Blob_GetBufferSize(v), NULL, &m_show_vs)) &&
                 SUCCEEDED(ID3D11Device_CreatePixelShader(m_dev, ID3D10Blob_GetBufferPointer(p), ID3D10Blob_GetBufferSize(p), NULL, &m_show_ps)) &&
                 SUCCEEDED(ID3D11Device_CreateBlendState(m_dev, &bd, &m_show_blend)) && (m_cb_show = cbuf(16));
        if (v) ID3D10Blob_Release(v);
        if (p) ID3D10Blob_Release(p);
        if (!ok) { m_log("motion view: shaders not made - off"); m_show_bad = 1; return; }
    }
    // the back buffer's view each frame: a kept reference would make the game's ResizeBuffers fail
    ID3D11RenderTargetView *rtv = NULL;
    D3D11_TEXTURE2D_DESC bdsc;
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)bb, &bdsc);
    if (FAILED(ID3D11Device_CreateRenderTargetView(m_dev, bb, NULL, &rtv))) return;
    r2m_internal = 1;
    ID3DDeviceContextState *game = NULL;
    ID3D11DeviceContext1_SwapDeviceContextState(m_ctx1, m_state, &game);
    ID3D11DeviceContext *c = m_ctx;
    float sz[4] = {(float)rw, (float)rh, (float)bdsc.Width, (float)bdsc.Height};
    ID3D11DeviceContext_UpdateSubresource(c, (ID3D11Resource *)m_cb_show, 0, NULL, sz, 0, 0);
    D3D11_VIEWPORT vp = {0, 0, (float)bdsc.Width, (float)bdsc.Height, 0, 1};
    ID3D11DeviceContext_OMSetRenderTargets(c, 1, &rtv, NULL);   // first: the own-motion texture is the pass's target in this state
    ID3D11DeviceContext_OMSetBlendState(c, m_show_blend, NULL, 0xffffffff);
    ID3D11DeviceContext_OMSetDepthStencilState(c, NULL, 0);
    ID3D11DeviceContext_RSSetState(c, m_rs);
    ID3D11DeviceContext_RSSetViewports(c, 1, &vp);
    ID3D11DeviceContext_IASetInputLayout(c, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(c, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(c, m_show_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(c, m_show_ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(c, 0, 1, &m_cb_show);
    ID3D11DeviceContext_PSSetShaderResources(c, 0, 1, &m_own_srv);
    ID3D11DeviceContext_Draw(c, 3, 0);
    ID3D11ShaderResourceView *none = NULL;
    ID3D11DeviceContext_PSSetShaderResources(c, 0, 1, &none);
    ID3D11DeviceContext_OMSetBlendState(c, NULL, NULL, 0xffffffff);   // the pass draws without blending
    ID3D11DeviceContext_OMSetRenderTargets(c, 0, NULL, NULL);
    ID3D11DeviceContext1_SwapDeviceContextState(m_ctx1, game, NULL);
    if (game) ID3DDeviceContextState_Release(game);
    r2m_internal = 0;
    ID3D11RenderTargetView_Release(rtv);
}
#endif

void r2m_report(void (*log)(const char *fmt, ...)) {
    if (!m_on) return;
    log("  motion pass: %u passes, %u animated draws recorded (%u skipped), %u records, texture changes %u", m_stat_passes, m_stat_draws,
        m_stat_skipped, m_stat_recs, m_stat_tex);
    m_stat_passes = m_stat_draws = m_stat_skipped = m_stat_recs = m_stat_found = m_stat_tex = 0;
}
