// Test for the jittered render's edges (fgbridge.c jit_edge_fill and the depth pass), no window: a player saw white lines
// along the right edge with DLAA while the camera moved. A scene drawn with the viewport shifted by the jitter, as
// bridgeprobe.c does, into a target cleared to the "sky"; with the jitter below 0 the last column / row stays the sky
// (measured, printed). After the HUD-less copy and jit_edge_fill no sky pixel may be left in the copy, and the depth pass
// must give the scene's depth there, not the cleared far plane. Jitter 0 and above: the copy is the render, unchanged.
#ifndef FGB_SRC
#define FGB_SRC "../fgbridge.c"   // build.sh: broken copies in build/
#endif
#include FGB_SRC
#define CHECK(c, ...) do { if (!(c)) { printf("FAILED: " __VA_ARGS__); putchar('\n'); return 1; } } while (0)
enum { W = 64, H = 36 };
static const char *hlsl =
    "float4 vs(uint i : SV_VertexID) : SV_Position { float2 t = float2((i << 1) & 2, i & 2); return float4(t * float2(2, -2) + float2(-1, 1), 0.5, 1); }\n"
    "float4 ps(float4 p : SV_Position) : SV_Target { return float4(0, 1, 0, 1); }\n";
static ID3DBlob *comp(pD3DCompile c, const char *src, const char *e, const char *t) {
    ID3DBlob *b = NULL, *err = NULL;
    if (FAILED(c(src, strlen(src), "t", NULL, NULL, e, t, 0, 0, &b, &err))) { if (err) printf("%s\n", (char *)ID3D10Blob_GetBufferPointer(err)); return NULL; }
    return b;
}
static ID3D11Texture2D *stage(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *t, D3D11_MAPPED_SUBRESOURCE *m) {
    D3D11_TEXTURE2D_DESC d; ID3D11Texture2D_GetDesc(t, &d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ID3D11Texture2D *s; ID3D11Device_CreateTexture2D(dev, &d, NULL, &s);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)s, (ID3D11Resource *)t);
    ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)s, 0, D3D11_MAP_READ, 0, m);
    return s;
}
static int is_sky(const uint8_t *p) { return p[0] == 115 && p[1] == 153 && p[2] == 217; }

int main(void) {
    ID3D11Device *dev; ID3D11DeviceContext *ctx;
    CHECK(SUCCEEDED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, NULL, &ctx)), "no D3D11 device");
    pD3DCompile c = (pD3DCompile)(void *)GetProcAddress(LoadLibraryA("d3dcompiler_47.dll"), "D3DCompile");
    ID3DBlob *vb = comp(c, hlsl, "vs", "vs_5_0"), *pb = comp(c, hlsl, "ps", "ps_5_0"), *cb_ = comp(c, cs_src, "cs", "cs_5_0");
    CHECK(vb && pb && cb_, "shaders");
    ID3D11VertexShader *vs; ID3D11PixelShader *ps; ID3D11ComputeShader *cs;
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vb), ID3D10Blob_GetBufferSize(vb), NULL, &vs);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(pb), ID3D10Blob_GetBufferSize(pb), NULL, &ps);
    ID3D11Device_CreateComputeShader(dev, ID3D10Blob_GetBufferPointer(cb_), ID3D10Blob_GetBufferSize(cb_), NULL, &cs);
    // the game's back buffer and depth (R24G8 typeless, as Rome 2's), the HUD-less copy, the depth pass's outputs
    D3D11_TEXTURE2D_DESC td = {W, H, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0};
    ID3D11Texture2D *bb, *hud, *ds, *dcopy, *d32, *mv;
    ID3D11Device_CreateTexture2D(dev, &td, NULL, &bb);
    td.BindFlags = 0; ID3D11Device_CreateTexture2D(dev, &td, NULL, &hud);
    td.Format = DXGI_FORMAT_R24G8_TYPELESS; td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ID3D11Device_CreateTexture2D(dev, &td, NULL, &ds);
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE; ID3D11Device_CreateTexture2D(dev, &td, NULL, &dcopy);
    td.Format = DXGI_FORMAT_R32_FLOAT; td.BindFlags = D3D11_BIND_UNORDERED_ACCESS; ID3D11Device_CreateTexture2D(dev, &td, NULL, &d32);
    td.Format = DXGI_FORMAT_R16G16_FLOAT; ID3D11Device_CreateTexture2D(dev, &td, NULL, &mv);
    ID3D11RenderTargetView *rtv; ID3D11DepthStencilView *dsv; ID3D11ShaderResourceView *srv; ID3D11UnorderedAccessView *uav[2];
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)bb, NULL, &rtv);
    D3D11_DEPTH_STENCIL_VIEW_DESC dvd = {DXGI_FORMAT_D24_UNORM_S8_UINT, D3D11_DSV_DIMENSION_TEXTURE2D, 0, {{0}}};
    ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource *)ds, &dvd, &dsv);
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {0};
    sd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
    ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)dcopy, &sd, &srv);
    ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)d32, NULL, &uav[0]);
    ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)mv, NULL, &uav[1]);
    D3D11_BUFFER_DESC bd = {144, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    ID3D11Buffer *cbuf; ID3D11Device_CreateBuffer(dev, &bd, NULL, &cbuf);
    D3D11_RASTERIZER_DESC rsd = {D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 0, 0, 0, TRUE, FALSE, FALSE, FALSE};
    ID3D11RasterizerState *rs; ID3D11Device_CreateRasterizerState(dev, &rsd, &rs);

    static const float jit[][2] = {{-0.25f, 0}, {0, -0.25f}, {-0.438f, 0.389f}, {-0.125f, -0.278f}, {0.375f, 0.056f}, {0, 0}, {0.25f, -0.389f}};
    int bad = 0;
    for (unsigned k = 0; k < sizeof jit / sizeof jit[0]; k++) {
        float jx = jit[k][0], jy = jit[k][1];
        const float sky[4] = {0.45f, 0.6f, 0.85f, 1};
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, sky);
        ID3D11DeviceContext_ClearDepthStencilView(ctx, dsv, D3D11_CLEAR_DEPTH, 1, 0);
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, dsv);
        D3D11_VIEWPORT v = {jx, jy, W, H, 0, 1};   // bridgeprobe.c: TopLeftX / Y += the jitter, the size kept
        ID3D11DeviceContext_RSSetViewports(ctx, 1, &v);
        ID3D11DeviceContext_RSSetState(ctx, rs);
        ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
        ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0); ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
        ID3D11DeviceContext_Draw(ctx, 3, 0);
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, NULL);
        // what fgb_hudless_ does: the render's copy, the edge fill, the depth pass
        D3D11_BOX box = {0, 0, 0, W, H, 1};
        ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)hud, 0, 0, 0, 0, (ID3D11Resource *)bb, 0, &box);
        jit_edge_fill(ctx, (ID3D11Resource *)hud, (ID3D11Resource *)bb, W, H, jx, jy);
        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)dcopy, (ID3D11Resource *)ds);
        D3D11_MAPPED_SUBRESOURCE m;
        ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)cbuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
        float *f = m.pData;
        memset(f, 0, 144);
        for (int i = 0; i < 4; i++) f[i * 5] = f[16 + i * 5] = 1;   // identity cameras
        f[32] = W; f[33] = H; f[34] = jx; f[35] = jy;
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)cbuf, 0);
        ID3D11DeviceContext_CSSetShader(ctx, cs, NULL, 0);
        ID3D11DeviceContext_CSSetShaderResources(ctx, 0, 1, &srv);
        ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 2, uav, NULL);
        ID3D11DeviceContext_CSSetConstantBuffers(ctx, 0, 1, &cbuf);
        ID3D11DeviceContext_Dispatch(ctx, (W + 7) / 8, (H + 7) / 8, 1);
        ID3D11UnorderedAccessView *nu[2] = {0}; ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 2, nu, NULL);

        D3D11_MAPPED_SUBRESOURCE mr, mh, md;
        ID3D11Texture2D *sr = stage(dev, ctx, bb, &mr), *sh = stage(dev, ctx, hud, &mh), *sdp = stage(dev, ctx, d32, &md);
        int col = 0, row = 0, left = 0, sky_copy = 0, far_depth = 0, changed = 0;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                const uint8_t *r = (const uint8_t *)mr.pData + y * mr.RowPitch + x * 4, *h = (const uint8_t *)mh.pData + y * mh.RowPitch + x * 4;
                float z = ((const float *)((const uint8_t *)md.pData + y * md.RowPitch))[x];
                if (is_sky(r)) { if (x == W - 1) col++; else if (y == H - 1) row++; else left++; }
                sky_copy += is_sky(h);
                far_depth += z > 0.75f;
                changed += memcmp(r, h, 4) != 0;
            }
        printf("  jitter (%6.3f, %6.3f): render's sky pixels: last column %d of %d, last row %d, elsewhere %d; HUD-less copy: sky %d, "
               "changed %d; depth pass: far depth %d\n", jx, jy, col, H, row, left, sky_copy, changed, far_depth);
        int edge = jx < 0 || jy < 0;
        if (sky_copy || far_depth) { printf("FAILED: the undrawn edge reached the HUD-less copy / depth\n"); bad = 1; }
        if (!edge && changed) { printf("FAILED: the copy changed without a jitter below 0\n"); bad = 1; }
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)sr, 0); ID3D11Texture2D_Release(sr);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)sh, 0); ID3D11Texture2D_Release(sh);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)sdp, 0); ID3D11Texture2D_Release(sdp);
    }
    printf(bad ? "FAILED\n" : "all jitter edge checks passed\n");
    return bad;
}
