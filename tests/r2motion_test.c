// Test for r2motion.c, the per-object motion pass (dlss/DLSS_LOG.md 19, offline tests O3 / O4): a stand-in for the game
// draws instanced skinned quads (2-bone and 4-bone layouts, 88-byte records at a buffer offset, a shared pose texture)
// with the game's skinning (the pass's own skin() / place(), which r2skintest proved bit-identical to the game's
// shaders), the pass records them and writes the motion where the depth is theirs. Checked against motion worked out
// on the CPU (orthographic camera, so the motion is the same over a whole quad):
//  - O3: motion in pixels, previous minus current, through a jittered viewport, both layouts' bone weights
//  - O4: history by position: records in another order, pose rows changed, the nearer of two candidates, a new model
//    (camera only), a model where the previous frame had another type (no history), an occluded model (no motion: the
//    depth is not its), a frame without the pass (the next has no history)
#include "../r2motion.c"
#include <stdarg.h>
#include <stdio.h>
#include <math.h>

#define W 128
#define H 64
#define AW 12   // pose texture: 4 bones x 3 texels per row, 8 rows (a power of two, like the game's 1024)
#define AH 8
#define CHECK(c, ...) do { if (!(c)) { printf("FAILED: " __VA_ARGS__); putchar('\n'); return 1; } } while (0)
static void tlog(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vprintf(fmt, a);
    va_end(a);
    putchar('\n');
}
// pose row r, bone b: the identity turned plus a shift
static float bone_t(int r, int b, int k) { return k == 0 ? 0.01f * r + 0.02f * b : k == 1 ? 0.005f * r - 0.01f * b : 0; }
// the camera: x in [-1, 3] and y in [-1, 1] across the screen, depth 0.5 + 0.01 x, shifted by tx (clip units)
static void camera(float *cam, float tx) {
    memset(cam, 0, 52 * sizeof *cam);
    const float m[16] = {0.5f, 0, 0, -0.5f + tx, 0, 1, 0, 0, 0.01f, 0, 0, 0.5f, 0, 0, 0, 1};
    memcpy(cam + 36, m, sizeof m);
}
typedef struct { float x, y; int row, type; } Inst;
// the skinned quad centre's shift (both layouts' fixed bone weights, as the vertex data below)
static void skin_off(int type, int row, float *o) {
    for (int k = 0; k < 2; k++) {
        if (type == 0) { float wa = 64 / 255.0f; o[k] = wa * bone_t(row, 0, k) + (1 - wa) * bone_t(row, 1, k); }
        else {
            float x = 26 / 255.0f, y = 51 / 255.0f, z = 77 / 255.0f, wl = 1 - ((y + z) + x);
            o[k] = x * bone_t(row, 2, k) + y * bone_t(row, 1, k) + z * bone_t(row, 0, k) + wl * bone_t(row, 3, k);
        }
    }
}
static void clip_of(const Inst *i, float tx, float *c) {
    float o[2];
    skin_off(i->type, i->row, o);
    c[0] = 0.5f * (i->x + o[0]) - 0.5f + tx;
    c[1] = i->y + o[1];
}
static float h2f(unsigned short h) {
    unsigned e = (h >> 10) & 31, m = h & 1023;
    float v = e ? ldexpf((float)(m | 1024), (int)e - 25) : ldexpf((float)m, -24);
    return h >> 15 ? -v : v;
}

static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static ID3D11Texture2D *mv, *mv_st, *ds;
static ID3D11DepthStencilView *dsv;
static ID3D11RenderTargetView *mv_rtv;
static ID3D11VertexShader *gvs[2], *occ_vs;
static ID3D11InputLayout *gil[2];
static D3D11_INPUT_ELEMENT_DESC ge[2][7];
static UINT gne[2];
static ID3D11Buffer *mesh[2], *ib, *recs[2], *gcam, *gasz;
static ID3D11ShaderResourceView *anim_srv;
static ID3D11SamplerState *pt;
static ID3D11DepthStencilState *less;
static float mvbuf[H][W][2];
static int owned[H][W];

// one frame of the stand-in game: per type one instanced draw of its records (a junk record first, skipped by the start
// instance), then the occluder if asked; then the pass (or not)
static void frame(const Inst *in, int n, float tx, float jx, float jy, int occlude, int pass) {
    float cam[52];
    r2m_frame_end();   // the last frame's Present (the glue calls it after each one)
    camera(cam, tx);
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)gcam, 0, NULL, cam, 0, 0);
    float zero[4] = {0, 0, 0, 0};
    ID3D11DeviceContext_ClearRenderTargetView(ctx, mv_rtv, zero);
    ID3D11DeviceContext_ClearDepthStencilView(ctx, dsv, D3D11_CLEAR_DEPTH, 1, 0);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, dsv);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, less, 0);
    D3D11_VIEWPORT vp = {jx, jy, W, H, 0, 1};
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_PSSetShader(ctx, NULL, NULL, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11Buffer *cbs[4] = {NULL, gcam, NULL, gasz};
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 4, cbs);
    ID3D11DeviceContext_VSSetShaderResources(ctx, 0, 1, &anim_srv);
    ID3D11DeviceContext_VSSetSamplers(ctx, 0, 1, &pt);
    ID3D11DeviceContext_IASetIndexBuffer(ctx, ib, DXGI_FORMAT_R16_UINT, 0);
    for (int t = 0; t < 2; t++) {
        float r[64][22] = {{0}};
        int k = 1;
        for (int i = 0; i < n; i++)
            if (in[i].type == t) {
                float *q = r[k++];
                q[0] = 1; q[3] = in[i].x; q[5] = 1; q[7] = in[i].y; q[10] = 1; q[12] = (float)in[i].row;
            }
        r[0][3] = 77;   // the junk record
        D3D11_MAPPED_SUBRESOURCE m;
        ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)recs[t], 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
        memcpy((char *)m.pData + 88, r, (size_t)k * 88);   // at an offset, like the game's rotating buffers
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)recs[t], 0);
        if (k == 1) continue;
        ID3D11Buffer *vbs[2] = {mesh[t], recs[t]};
        UINT st[2] = {t ? 20u : 16u, 88}, of[2] = {0, 88};
        ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 2, vbs, st, of);
        ID3D11DeviceContext_IASetInputLayout(ctx, gil[t]);
        ID3D11DeviceContext_VSSetShader(ctx, gvs[t], NULL, 0);
        r2m_draw(ctx, t, gil[t], ge[t], gne[t], 1, 6, (UINT)k - 1, 0, 0, 1);
        ID3D11DeviceContext_DrawIndexedInstanced(ctx, 6, (UINT)k - 1, 0, 0, 1);
    }
    if (occlude) {
        ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
        ID3D11DeviceContext_VSSetShader(ctx, occ_vs, NULL, 0);
        ID3D11DeviceContext_Draw(ctx, 3, 0);
    }
    // which pixels the models own in the depth (not cleared, not the occluder's 0.1)
    static ID3D11Texture2D *dst;
    D3D11_TEXTURE2D_DESC q = {W, H, 1, 1, DXGI_FORMAT_R24G8_TYPELESS, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
    if (!dst) ID3D11Device_CreateTexture2D(dev, &q, NULL, &dst);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)dst, (ID3D11Resource *)ds);
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)dst, 0, D3D11_MAP_READ, 0, &m);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            unsigned d = ((const unsigned *)((const char *)m.pData + y * m.RowPitch))[x] & 0xFFFFFF;
            owned[y][x] = d != 0xFFFFFF && d > 0x400000;
        }
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)dst, 0);
    if (pass) r2m_pass(dsv, cam, W, H, jx, jy, 0, 1);   // (without: the draws stay until the next frame_end)
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)mv_st, (ID3D11Resource *)mv);
    ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)mv_st, 0, D3D11_MAP_READ, 0, &m);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            for (int c = 0; c < 2; c++) mvbuf[y][x][c] = h2f(((const unsigned short *)((const char *)m.pData + y * m.RowPitch))[x * 2 + c]);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)mv_st, 0);
}
// the motion at the centre of instance `cur` (this frame, camera tx1, jitter) must be from `prev` (camera tx0)
static int expect(const char *what, const Inst *cur, float tx1, float jx, float jy, const Inst *prev, float tx0, int zero) {
    float c[2], p[2];
    clip_of(cur, tx1, c);
    clip_of(prev, tx0, p);
    int px = (int)floorf((c[0] + 1) * 0.5f * W + jx), py = (int)floorf((1 - c[1]) * 0.5f * H + jy);
    float ex = zero ? 0 : (p[0] - c[0]) * 0.5f * W, ey = zero ? 0 : (p[1] - c[1]) * -0.5f * H;
    float gx = mvbuf[py][px][0], gy = mvbuf[py][px][1];
    int ok = fabsf(gx - ex) < 0.02f && fabsf(gy - ey) < 0.02f;
    printf("  %-44s pixel %3d,%2d: motion %8.4f %8.4f, expected %8.4f %8.4f%s\n", what, px, py, gx, gy, ex, ey, ok ? "" : "  <-- WRONG");
    return ok;
}

#ifndef PROFILING   // the release form: no debug view, so no own-motion target to check (the frames run with it off only)
#define VIEWS 1
#define r2m_set_view(on) ((void)(on))
static int own_expect(const char *what, const Inst *cur, const Inst *prev, float tx, float jx, float jy, int want_w) {
    (void)what; (void)cur; (void)prev; (void)tx; (void)jx; (void)jy; (void)want_w;
    return 1;
}
#else
#define VIEWS 2
// the debug view's texture at the centre of `cur`: the model's own motion (from `prev`, both through this frame's camera),
// w = 1 where a model was drawn; want_w 0: nothing there
static int own_expect(const char *what, const Inst *cur, const Inst *prev, float tx, float jx, float jy, int want_w) {
    static ID3D11Texture2D *st;
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D_GetDesc(m_own, &d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (!st) ID3D11Device_CreateTexture2D(dev, &d, NULL, &st);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)m_own);
    float c[2], p[2];
    clip_of(cur, tx, c);
    clip_of(prev, tx, p);
    int px = (int)floorf((c[0] + 1) * 0.5f * W + jx), py = (int)floorf((1 - c[1]) * 0.5f * H + jy);
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m);
    const unsigned short *h = (const unsigned short *)((const char *)m.pData + py * m.RowPitch) + px * 4;
    float gx = h2f(h[0]), gy = h2f(h[1]), gw = h2f(h[3]);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
    float ex = want_w ? (p[0] - c[0]) * 0.5f * W : 0, ey = want_w ? (p[1] - c[1]) * -0.5f * H : 0;
    int ok = fabsf(gx - ex) < 0.02f && fabsf(gy - ey) < 0.02f && gw == (float)want_w;
    printf("  own motion: %-37s pixel %3d,%2d: %8.4f %8.4f w %g, expected %8.4f %8.4f w %d%s\n", what, px, py, gx, gy, gw, ex, ey, want_w,
           ok ? "" : "  <-- WRONG");
    return ok;
}
#endif

int main(void) {
    typedef HRESULT(WINAPI * create_fn)(void *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
    create_fn create = (create_fn)(void *)GetProcAddress(LoadLibraryW(L"d3d11.dll"), "D3D11CreateDevice");
    CHECK(SUCCEEDED(create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_SINGLETHREADED, NULL, 0, D3D11_SDK_VERSION, &dev, NULL, &ctx)), "device");
    D3D11_TEXTURE2D_DESC td = {W, H, 1, 1, DXGI_FORMAT_R16G16_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0};
    ID3D11Device_CreateTexture2D(dev, &td, NULL, &mv);
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)mv, NULL, &mv_rtv);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Device_CreateTexture2D(dev, &td, NULL, &mv_st);
    td.Format = DXGI_FORMAT_R24G8_TYPELESS; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_DEPTH_STENCIL; td.CPUAccessFlags = 0;
    CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &ds)), "depth");
    D3D11_DEPTH_STENCIL_VIEW_DESC dvd = {DXGI_FORMAT_D24_UNORM_S8_UINT, D3D11_DSV_DIMENSION_TEXTURE2D, 0, {{0}}};
    CHECK(SUCCEEDED(ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource *)ds, &dvd, &dsv)), "depth view");
    CHECK(r2m_init(dev, ctx, mv, tlog), "r2m_init");

    // the stand-in game's shaders: the pass's skinning, written to SV_Position; an occluder near the camera
    static const char game[] =
        "struct GIn { float4 pos : POSITION0; float4 bi : POSITION1;\n"
        "#ifdef SKIN4\n"
        "  float4 bw : POSITION2;\n"
        "#endif\n"
        "  float4 w0 : TEXCOORD0; float4 w1 : TEXCOORD1; float4 w2 : TEXCOORD2; float4 t3 : TEXCOORD3; };\n"
        "float4 game(GIn g) : SV_Position { VIn i = (VIn)0; i.pos = g.pos; i.bi = g.bi;\n"
        "#ifdef SKIN4\n"
        "  i.bw = g.bw;\n"
        "#endif\n"
        "  i.w0 = g.w0; i.w1 = g.w1; i.w2 = g.w2; i.t3 = g.t3; return place(skin(anim, asz, i, i.t3.x), i.w0, i.w1, i.w2, vp); }\n"
        "float4 occ(uint v : SV_VertexID) : SV_Position { float2 t = float2(v == 2 ? 1.0 : -0.3, v == 1 ? 1.0 : -0.3); return float4(t, 0.1, 1); }\n";
    static char src[sizeof r2motion_hlsl + sizeof game];
    snprintf(src, sizeof src, "%s%s", r2motion_hlsl, game);
    comp_fn comp = (comp_fn)(void *)GetProcAddress(LoadLibraryExA("d3dcompiler_47.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32), "D3DCompile");
    D3D_SHADER_MACRO s4[2] = {{"SKIN4", "1"}, {NULL, NULL}};
    ID3DBlob *b[3], *err = NULL;
    for (int t = 0; t < 3; t++) {
        HRESULT hr = comp(src, strlen(src), "game", t == 1 ? s4 : NULL, NULL, t == 2 ? "occ" : "game", "vs_5_0", 0, 0, &b[t], &err);
        CHECK(SUCCEEDED(hr), "game shader %d: %s", t, err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
    }
    for (int t = 0; t < 2; t++) ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(b[t]), ID3D10Blob_GetBufferSize(b[t]), NULL, &gvs[t]);
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(b[2]), ID3D10Blob_GetBufferSize(b[2]), NULL, &occ_vs);
    // layouts: mesh (position, bone bytes [, weight bytes]) in slot 0, records in slot 1 (stride 88, the rest unread)
    static const D3D11_INPUT_ELEMENT_DESC inst[4] = {{"TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
                                                     {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
                                                     {"TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1},
                                                     {"TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48, D3D11_INPUT_PER_INSTANCE_DATA, 1}};
    for (int t = 0; t < 2; t++) {
        D3D11_INPUT_ELEMENT_DESC v[3] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
                                         {"POSITION", 1, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
                                         {"POSITION", 2, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0}};
        gne[t] = 0;
        for (int k = 0; k < 2 + t; k++) ge[t][gne[t]++] = v[k];
        for (int k = 0; k < 4; k++) ge[t][gne[t]++] = inst[k];
        CHECK(SUCCEEDED(ID3D11Device_CreateInputLayout(dev, ge[t], gne[t], ID3D10Blob_GetBufferPointer(b[t]), ID3D10Blob_GetBufferSize(b[t]), &gil[t])), "layout %d", t);
        // a quad 0.2 m wide; 2 bones: A = bone 0 (byte .z) weight 64/255 (.x), B = bone 1 (.y); 4 bones: bytes 2, 1, 0, 3, weights 26, 51, 77
        unsigned char vb[4][20];
        for (int k = 0; k < 4; k++) {
            float p[3] = {k & 1 ? 0.1f : -0.1f, k & 2 ? 0.1f : -0.1f, 0};
            memcpy(vb[k], p, 12);
            const unsigned char b2[8] = {64, 1, 0, 0, 0, 0, 0, 0}, b4[8] = {2, 1, 0, 3, 26, 51, 77, 0};
            memcpy(vb[k] + 12, t ? b4 : b2, 8);
        }
        unsigned char packed[80];
        for (int k = 0; k < 4; k++) memcpy(packed + k * (t ? 20 : 16), vb[k], t ? 20 : 16);
        D3D11_BUFFER_DESC bd = {t ? 80u : 64u, D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
        D3D11_SUBRESOURCE_DATA sd = {packed, 0, 0};
        ID3D11Device_CreateBuffer(dev, &bd, &sd, &mesh[t]);
        D3D11_BUFFER_DESC rd = {64 * 88 + 176, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
        ID3D11Device_CreateBuffer(dev, &rd, NULL, &recs[t]);
    }
    unsigned short idx[6] = {0, 2, 1, 2, 3, 1};   // clockwise on screen (the default rasterizer culls the others)
    D3D11_BUFFER_DESC bd = {sizeof idx, D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER, 0, 0, 0};
    D3D11_SUBRESOURCE_DATA sd = {idx, 0, 0};
    ID3D11Device_CreateBuffer(dev, &bd, &sd, &ib);
    float asz[4] = {AH - 1, 1.0f / AH, AW - 1, 1.0f / (AW - 1)};   // the game's variant_animation_size
    bd.ByteWidth = 16; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; sd.pSysMem = asz;
    ID3D11Device_CreateBuffer(dev, &bd, &sd, &gasz);
    bd.ByteWidth = 208; bd.Usage = D3D11_USAGE_DEFAULT;
    ID3D11Device_CreateBuffer(dev, &bd, NULL, &gcam);
    static float at[AH][AW][4];
    for (int r = 0; r < AH; r++)
        for (int bn = 0; bn < 4; bn++)
            for (int k = 0; k < 3; k++) { float *q = at[r][bn * 3 + k]; q[k] = 1; q[3] = bone_t(r, bn, k); }
    D3D11_TEXTURE2D_DESC ad = {AW, AH, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
    D3D11_SUBRESOURCE_DATA asd = {at, AW * 16, 0};
    ID3D11Texture2D *atex;
    ID3D11Device_CreateTexture2D(dev, &ad, &asd, &atex);
    ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)atex, NULL, &anim_srv);
    D3D11_SAMPLER_DESC smp = {D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP,
                              0, 1, D3D11_COMPARISON_NEVER, {0, 0, 0, 0}, 0, D3D11_FLOAT32_MAX};
    ID3D11Device_CreateSamplerState(dev, &smp, &pt);
    D3D11_DEPTH_STENCIL_DESC dd = {TRUE, D3D11_DEPTH_WRITE_MASK_ALL, D3D11_COMPARISON_LESS, FALSE, 0, 0, {1, 1, 1, 8}, {1, 1, 1, 8}};
    CHECK(SUCCEEDED(ID3D11Device_CreateDepthStencilState(dev, &dd, &less)), "depth state");

    // frame 0: A, B, P, Q (2 bones), Y, M (4 bones)
    const Inst A = {0, 0, 0, 0}, B = {1, 0, 1, 0}, P = {2, 0.5f, 2, 0}, Q = {2, 0.8f, 6, 0}, Y = {0, -0.6f, 5, 1}, M = {1, -0.6f, 0, 1};
    const Inst f0[] = {A, B, P, Q, Y, M};
    // frame 1, other order: A and B moved, A on another pose row; R near P (nearer than Q); N new; X a 2-bone model where
    // Y (4 bones) was; M moved, another row; B under the occluder; camera shifted
    const Inst A1 = {0.05f, 0, 3, 0}, B1 = {1, 0.04f, 1, 0}, R = {2, 0.6f, 4, 0}, N = {0, 0.6f, 3, 0}, X = {0, -0.6f, 0, 0}, M1 = {1.03f, -0.6f, 2, 1};
    const Inst f1[] = {B1, N, R, A1, Q, X, M1};
    const float tx = 0.01f, jx = -0.1f, jy = 0.4f;
    int ok = 1;
    // twice: debug view off (the shaders the game normally runs), then on (the variants that also write the own motion)
    for (int view = 0; view < VIEWS; view++) {
    printf("  debug view %s:\n", view ? "on" : "off");
    r2m_set_view(view);
    frame(f0, 6, 0, 0.3f, -0.2f, 0, 1);
    frame(f1, 7, tx, jx, jy, 1, 1);
    ok &= expect("moved, other pose row (2 bones)", &A1, tx, jx, jy, &A, 0, 0);
    ok &= expect("the nearer of two candidates", &R, tx, jx, jy, &P, 0, 0);
    ok &= expect("unmoved (camera only)", &Q, tx, jx, jy, &Q, 0, 0);
    Inst Nc = N, Xc = X;   // no history: this frame's pose and place through the previous camera
    ok &= expect("new model (camera only)", &N, tx, jx, jy, &Nc, 0, 0);
    ok &= expect("other type there before (camera only)", &X, tx, jx, jy, &Xc, 0, 0);
    ok &= expect("moved, other pose row (4 bones)", &M1, tx, jx, jy, &M, 0, 0);
    ok &= expect("occluded (depth not its: nothing written)", &B1, tx, jx, jy, &B, 0, 1);
    // every model pixel has motion (all moved at least with the camera): the pass's depth is the game's, bit for bit;
    // nothing elsewhere (background, the occluder over B)
    int own = 0, miss = 0, leak = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int moved = mvbuf[y][x][0] != 0 || mvbuf[y][x][1] != 0;
            own += owned[y][x];
            miss += owned[y][x] && !moved;
            leak += !owned[y][x] && moved;
        }
    printf("  model pixels %d: without motion %d; pixels with motion outside the models %d\n", own, miss, leak);
    ok &= own > 200 && !miss && !leak;
    if (!view) continue;
    // the debug view's own motion (the camera's shift left out): the moved models' alone; none for a new or still model
    // (but marked as a model); nothing under the occluder or in the background
    ok &= own_expect("moved, other pose row (2 bones)", &A1, &A, tx, jx, jy, 1);
    ok &= own_expect("moved, other pose row (4 bones)", &M1, &M, tx, jx, jy, 1);
    ok &= own_expect("unmoved", &Q, &Q, tx, jx, jy, 1);
    ok &= own_expect("new model", &N, &N, tx, jx, jy, 1);
    ok &= own_expect("occluded", &B1, &B1, tx, jx, jy, 0);
    }
    // frame 2 recorded but no pass (no HUD-less point), frame 3: no history, so no object motion even with A moved
    frame(f1, 7, tx, 0, 0, 0, 0);
    ok &= own_expect("frame without the pass: cleared", &A1, &A1, tx, 0, 0, 0);
    const Inst A3 = {0.1f, 0, 5, 0};
    const Inst f3[] = {A3, Q};
    frame(f3, 2, tx, 0, 0, 0, 1);
    ok &= expect("after a frame without the pass (no history)", &A3, tx, 0, 0, &A3, tx, 1);
    ok &= own_expect("after a frame without the pass", &A3, &A3, tx, 0, 0, 1);
    r2m_report(tlog);
    CHECK(ok, "motion values");
    CHECK(ID3D11Device_GetDeviceRemovedReason(dev) == S_OK, "device removed");
    printf("r2motion test passed\n");
    return 0;
}
