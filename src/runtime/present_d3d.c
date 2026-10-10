/* present_d3d.c -- the GPU half of the presenter (present.c decides the rect).
 *
 * dos32 renders the screen through the palette into 32-bit pixels; they go
 * into a Direct3D 11 texture and a pixel shader scales them:
 *
 *   sharp    sharp-bilinear: whole texels stay crisp, only their edges blend,
 *            so a 4.5x upscale neither blurs nor shimmers (the default)
 *   smooth   bilinear
 *   crt      scanlines and an aperture-grille mask
 *   nearest  plain pixels;  integer: nearest at whole multiples only
 *
 * The device and shader code are gunman's present_d3d.c (same author, MIT).
 * Any failure returns 0 and present.c falls back to GDI.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "present.h"

typedef HRESULT (WINAPI *compile_fn)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR,
                                     UINT, UINT, ID3DBlob**, ID3DBlob**);

static const char k_hlsl[] =
"Texture2D t0 : register(t0);\n"
"SamplerState s_lin : register(s0);\n"
"SamplerState s_pt : register(s1);\n"
"cbuffer C : register(b0) { float2 src; float2 dst; int mode; int look; int2 pad; };\n"
"struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
"V vs(uint id : SV_VertexID) {\n"
"  V o; float2 p = float2((id << 1) & 2, id & 2);\n"
"  o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = p; return o;\n"
"}\n"
"float3 sharp(float2 uv) {\n"
"  float2 scale = max(dst / src, 1.0);\n"
"  float2 texel = uv * src, fl = floor(texel), c = frac(texel) - 0.5;\n"
"  float2 range = 0.5 - 0.5 / scale;\n"
"  float2 f = (c - clamp(c, -range, range)) * scale + 0.5;\n"
"  return t0.Sample(s_lin, (fl + f) / src).rgb;\n"
"}\n"
"float3 crt(float2 uv, float2 px) {\n"
"  /* Scanlines need a few screen rows per source row: below ~3x they beat\n"
"   * against the pixel grid (moire), so the effect fades in from 1.5x to 3x. */\n"
"  float k = saturate((dst.y / src.y - 1.5) / 1.5);\n"
"  float3 col = sharp(uv);\n"
"  float d = frac(uv.y * src.y) - 0.5;\n"
"  float beam = lerp(1.0, exp(-d * d * 8.0) * 1.6, k);\n"
"  uint m = (uint)px.x % 3u;\n"
"  float3 mask = m == 0u ? float3(1.25, 0.87, 0.87) : m == 1u ? float3(0.87, 1.25, 0.87)\n"
"                                                  : float3(0.87, 0.87, 1.25);\n"
"  return col * beam * lerp(float3(1, 1, 1), mask, k);\n"
"}\n"
"float4 ps(V i) : SV_Target {\n"
"  float3 c;\n"
"  if (mode == 0) c = sharp(i.uv);\n"
"  else if (mode == 1) c = t0.Sample(s_lin, i.uv).rgb;\n"
"  else if (mode == 2) c = crt(i.uv, i.pos.xy);\n"
"  else c = t0.Sample(s_pt, i.uv).rgb;\n"
"  if (look == 1) {\n"                                 /* vivid: saturation, a little contrast */
"    float l = dot(c, float3(0.299, 0.587, 0.114));\n"
"    c = lerp(float3(l, l, l), c, 1.3);\n"
"    c = saturate((c - 0.5) * 1.08 + 0.5);\n"
"  }\n"
"  return float4(saturate(c), 1);\n"
"}\n";

static struct {
    int failed;
    HWND hwnd;
    ID3D11Device* dev;
    ID3D11DeviceContext* ctx;
    IDXGISwapChain* sc;
    ID3D11RenderTargetView* rtv;
    ID3D11VertexShader* vs;
    ID3D11PixelShader* ps;
    ID3D11SamplerState* samp[2];
    ID3D11Buffer* cb;
    ID3D11Texture2D* tex;
    ID3D11ShaderResourceView* srv;
    int tw, th, cw, ch;
} d;

static int fail(const char* what, HRESULT hr) {
    fprintf(stderr, "[present] Direct3D 11 unavailable (%s, 0x%08lX): using GDI\n", what, (unsigned long)hr);
    d.failed = 1;
    return 0;
}

static int init(HWND hw) {
    DXGI_SWAP_CHAIN_DESC sd = {0};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hw;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_10_0;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &fl, 1,
                                               D3D11_SDK_VERSION, &sd, &d.sc, &d.dev, NULL, &d.ctx);
    if (FAILED(hr)) {   /* no GPU (or a remote session without one): WARP is still a GPU pipeline */
        hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, &fl, 1,
                                           D3D11_SDK_VERSION, &sd, &d.sc, &d.dev, NULL, &d.ctx);
        if (FAILED(hr)) return fail("device", hr);
    }
    /* Alt+Enter is ours (borderless), not DXGI's exclusive fullscreen. */
    IDXGIFactory* f = NULL;
    if (SUCCEEDED(IDXGISwapChain_GetParent(d.sc, &IID_IDXGIFactory, (void**)&f))) {
        IDXGIFactory_MakeWindowAssociation(f, hw, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        IDXGIFactory_Release(f);
    }

    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    compile_fn compile = dc ? (compile_fn)GetProcAddress(dc, "D3DCompile") : NULL;
    if (!compile) return fail("d3dcompiler_47.dll", 0);
    ID3DBlob *vb = NULL, *pb = NULL, *err = NULL;
    hr = compile(k_hlsl, sizeof k_hlsl - 1, "present", NULL, NULL, "vs", "vs_4_0", 0, 0, &vb, &err);
    if (SUCCEEDED(hr))
        hr = compile(k_hlsl, sizeof k_hlsl - 1, "present", NULL, NULL, "ps", "ps_4_0", 0, 0, &pb, &err);
    if (FAILED(hr)) {
        if (err) fprintf(stderr, "[present] %s\n", (const char*)ID3D10Blob_GetBufferPointer(err));
        return fail("shader", hr);
    }
    ID3D11Device_CreateVertexShader(d.dev, ID3D10Blob_GetBufferPointer(vb), ID3D10Blob_GetBufferSize(vb), NULL, &d.vs);
    ID3D11Device_CreatePixelShader(d.dev, ID3D10Blob_GetBufferPointer(pb), ID3D10Blob_GetBufferSize(pb), NULL, &d.ps);
    ID3D10Blob_Release(vb); ID3D10Blob_Release(pb);

    D3D11_SAMPLER_DESC s = {0};
    s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    s.MaxLOD = D3D11_FLOAT32_MAX;
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    ID3D11Device_CreateSamplerState(d.dev, &s, &d.samp[0]);
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    ID3D11Device_CreateSamplerState(d.dev, &s, &d.samp[1]);

    D3D11_BUFFER_DESC b = {0};
    b.ByteWidth = 32;
    b.Usage = D3D11_USAGE_DEFAULT;
    b.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Device_CreateBuffer(d.dev, &b, NULL, &d.cb);
    d.hwnd = hw;
    fprintf(stderr, "[present] Direct3D 11 presenter\n");
    return 1;
}

int d3d_present(HWND hw, const uint32_t* px, int w, int h, const RECT* r, int cw, int ch,
                int mode, int look, int vsync) {
    if (d.failed) return 0;
    if (!d.dev && !init(hw)) return 0;
    if (cw <= 0 || ch <= 0) return 1;              /* minimised */

    if (!d.tex || d.tw != w || d.th != h) {
        if (d.srv) ID3D11ShaderResourceView_Release(d.srv);
        if (d.tex) ID3D11Texture2D_Release(d.tex);
        D3D11_TEXTURE2D_DESC td = {0};
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8X8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(d.dev, &td, NULL, &d.tex))) return fail("texture", 0);
        ID3D11Device_CreateShaderResourceView(d.dev, (ID3D11Resource*)d.tex, NULL, &d.srv);
        d.tw = w; d.th = h;
    }
    ID3D11DeviceContext_UpdateSubresource(d.ctx, (ID3D11Resource*)d.tex, 0, NULL, px, w * 4, 0);

    if (!d.rtv || cw != d.cw || ch != d.ch) {
        if (d.rtv) { ID3D11RenderTargetView_Release(d.rtv); d.rtv = NULL; }
        ID3D11DeviceContext_OMSetRenderTargets(d.ctx, 0, NULL, NULL);
        if (FAILED(IDXGISwapChain_ResizeBuffers(d.sc, 0, cw, ch, DXGI_FORMAT_UNKNOWN, 0))) return 0;
        ID3D11Texture2D* back = NULL;
        IDXGISwapChain_GetBuffer(d.sc, 0, &IID_ID3D11Texture2D, (void**)&back);
        ID3D11Device_CreateRenderTargetView(d.dev, (ID3D11Resource*)back, NULL, &d.rtv);
        ID3D11Texture2D_Release(back);
        d.cw = cw; d.ch = ch;
    }

    struct { float src[2], dst[2]; int mode, look, pad[2]; } c = {
        { (float)w, (float)h }, { (float)(r->right - r->left), (float)(r->bottom - r->top) },
        mode == SCALE_INTEGER ? SCALE_NEAREST : mode, look, { 0, 0 } };
    ID3D11DeviceContext_UpdateSubresource(d.ctx, (ID3D11Resource*)d.cb, 0, NULL, &c, 0, 0);

    static const float black[4] = { 0, 0, 0, 1 };
    ID3D11DeviceContext_ClearRenderTargetView(d.ctx, d.rtv, black);
    D3D11_VIEWPORT vp = { (float)r->left, (float)r->top, (float)(r->right - r->left),
                          (float)(r->bottom - r->top), 0, 1 };
    ID3D11DeviceContext_RSSetViewports(d.ctx, 1, &vp);
    ID3D11DeviceContext_OMSetRenderTargets(d.ctx, 1, &d.rtv, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(d.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(d.ctx, d.vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(d.ctx, d.ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(d.ctx, 0, 1, &d.srv);
    ID3D11DeviceContext_PSSetSamplers(d.ctx, 0, 2, d.samp);
    ID3D11DeviceContext_PSSetConstantBuffers(d.ctx, 0, 1, &d.cb);
    ID3D11DeviceContext_Draw(d.ctx, 3, 0);
    IDXGISwapChain_Present(d.sc, vsync ? 1 : 0, 0);
    return 1;
}
