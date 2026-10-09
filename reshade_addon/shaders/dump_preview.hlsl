// DumpPreview.cs -- the frame dump's .jpg beside a .dds their WIC writer could not encode.
//
// Their Dump2DResource saves a .jpg where WIC can encode the format and a .dds otherwise: every BC
// texture, the HDR scene and lighting buffers, the 16-bit targets. A capture is read as pictures, so
// the GPU samples such a texture into R8G8B8A8 (the hardware decodes BC1-7, BC6H and the packed floats)
// and their own writer then encodes THAT. Floats are tone-mapped x/(1+x) and gamma 2.2 so an HDR buffer
// reads as a picture; everything else is written as sampled.
//
// Compiled with: fxc /T cs_5_0 /E main /O3 /Fo shaders_cso\DumpPreview.cso reshade_addon\shaders\dump_preview.hlsl

cbuffer Params : register(b0)
{
    uint2 gDim;       // width, height of the source's mip 0
    uint  gTonemap;   // 1 for float formats
    uint  _pad;
};

Texture2D<float4>   gSrc : register(t0);
RWTexture2D<float4> gDst : register(u0);   // R8G8B8A8_UNORM
SamplerState        gSmp : register(s0);   // point, clamp: one texel per thread, and Sample decodes BC where Load cannot

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gDim.x || id.y >= gDim.y) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(gDim);
    float4 c = gSrc.SampleLevel(gSmp, uv, 0);
    if (gTonemap) {
        float3 x = max(c.rgb, 0.0);
        x = x / (1.0 + x);
        c.rgb = pow(x, 1.0 / 2.2);
    }
    gDst[id.xy] = float4(c.rgb, 1.0);
}
