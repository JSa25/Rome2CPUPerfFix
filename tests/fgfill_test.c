// Test for the DLSS add-on's GPU work per frame (fgbridge.c, Attila audit 2026-11-20), on a WARP device without a helper:
// - the motion vector hole fill runs only while AMD FSR upscales or generates frames: with NVIDIA DLSS fgb_fill_begin
//   copies nothing and fgb_fill_end returns before its compute pass;
// - the reactive mask's scene copies take the render size (the shader reads nothing past it), not the whole target.
// usage: fgfill_test.exe
#ifndef FGB_SRC
#define FGB_SRC "../fgbridge.c"   // build.sh: broken copies in build/
#endif
#include FGB_SRC
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
enum { W = 64, H = 36, RW = 40, RH = 20 };

static ID3D11Texture2D *tex(DXGI_FORMAT f, UINT bind, const void *init, UINT pitch) {
    D3D11_TEXTURE2D_DESC d = {W, H, 1, 1, f, {1, 0}, D3D11_USAGE_DEFAULT, bind, 0, 0};
    D3D11_SUBRESOURCE_DATA s = {init, pitch, 0};
    ID3D11Texture2D *t = NULL;
    CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(g_dev, &d, init ? &s : NULL, &t)));
    return t;
}
// the first texel's first 4 bytes, and whether any texel at or past (x0, y0) is not zero
static unsigned read_tex(ID3D11Texture2D *t, UINT x0, UINT y0, int *outside) {
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D_GetDesc(t, &d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ID3D11Texture2D *s;
    CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &s)));
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)s, (ID3D11Resource *)t);
    D3D11_MAPPED_SUBRESOURCE m;
    CHECK(SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)s, 0, D3D11_MAP_READ, 0, &m)));
    unsigned first = *(const unsigned *)m.pData;
    *outside = 0;
    for (UINT y = 0; y < d.Height; y++)
        for (UINT x = 0; x < d.Width; x++)
            if ((x >= x0 || y >= y0) && ((const unsigned *)((const char *)m.pData + y * m.RowPitch))[x]) *outside = 1;
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)s, 0);
    ID3D11Texture2D_Release(s);
    return first;
}

int main(void) {
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    CHECK(SUCCEEDED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, &fl, 1, D3D11_SDK_VERSION, &g_dev, NULL, &g_ctx)));
    static unsigned ones[W * H];
    for (int i = 0; i < W * H; i++) ones[i] = 0x3C003C00;   // RG16F 1.0, 1.0
    static FgbShared sh;
    g_sh = &sh;
    sh.helper_ok = 1;
    g_fill_cs = (ID3D11ComputeShader *)(void *)1;   // set up; never reached with DLSS (fgb_fill_end returns first)

    // NVIDIA DLSS: no copy, no pass
    g_mv = tex(DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE, ones, W * 4);
    g_mvcam = tex(DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE, NULL, 0);
    unsigned before = g_fill_frames;
    sh.sr_backend = 1; sh.fg_backend = 1; sh.fg_max = 2;
    fgb_fill_begin();
    int out;
    CHECK(read_tex(g_mvcam, W, H, &out) == 0);
    fgb_fill_end(RW, RH);   // would bind the stand-in shader above: it must return first
    CHECK(g_fill_frames == before);
    printf("NVIDIA DLSS: no hole fill (no copy, no pass)\n");
    // AMD FSR upscaling, or AMD FSR frame generation with DLSS: the camera-only vectors are kept for the fill
    for (int k = 0; k < 2; k++) {
        ID3D11Texture2D_Release(g_mvcam);
        g_mvcam = tex(DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE, NULL, 0);
        sh.sr_backend = k ? 1 : 2; sh.fg_backend = k ? 2 : 1;
        fgb_fill_begin();
        CHECK(read_tex(g_mvcam, W, H, &out) == 0x3C003C00);
    }
    printf("AMD FSR (upscaling or frame generation): the fill's copy runs\n");

    // the reactive mask's copy of the scene: the render-size corner only
    g_w = W; g_h = H; g_sr = 1; g_rw = RW; g_rh = RH;
    ID3D11Texture2D *hdr = tex(DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, ones, W * 4), *copy = NULL;
    ID3D11ShaderResourceView *srv = NULL;
    CHECK(react_copy(&copy, &srv, (ID3D11Resource *)hdr));
    CHECK(read_tex(copy, RW, RH, &out) == 0x3C003C00 && !out);
    printf("reactive mask copy: %ux%u of %ux%u\n", RW, RH, W, H);
    printf("all fgfill tests passed\n");
    return 0;
}
