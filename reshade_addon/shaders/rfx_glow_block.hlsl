// rfx_glow_block.hlsl -- the SOURCE of the IR block rfx_dxil.cpp injects for the RabbitFX glow / hue
// (RabbitFX.ini [ShaderRegexMain.Pattern.Replace], the t50 block and the o0 / o3 output blocks). Not
// loaded at runtime: tools\gen_rfx_glow_ir.py compiles it with dxc, lifts the straight-line body out of
// the disassembly and writes reshade_addon\rfx_glow_ir.inc. It must stay BRANCHLESS (one basic block):
// the block is inserted at the entry of the game's pixel shader.
//
//   on      = the GlowMap is bound (width + height != 0) and its alpha at the pixel != 0
//   o0.xyz  = hueshift(map.rgb) * map.a * brightness     (scene colour: the emissive)
//   o3.xyz  = hueshift(map.rgb)                          (base colour)
// The patcher puts `select on, <this>, <the game's value>` in front of the game's six storeOutputs.
Texture2D<float4> GlowMap : register(t50);
Buffer<float4>    Params  : register(t52);   // [0] = x217..w217: hue turns, sat bias, val bias, brightness
SamplerState      Samp    : register(s0);

float3 rgb2hsv(float3 c)
{
    float4 K = float4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    // lerp / step, not ?: -- dxc turns the vector ternary into a branch with phis, and the block must be one.
    float4 p = lerp(float4(c.bg, K.wz), float4(c.gb, K.xy), step(c.b, c.g));
    float4 q = lerp(float4(p.xyw, c.r), float4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return float3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}
float3 hsv2rgb(float3 c)
{
    float4 K = float4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    float3 p = abs(frac(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * lerp(K.xxx, saturate(p - K.xxx), c.y);
}

struct Out
{
    float4 o0 : SV_Target0;
    float4 o3 : SV_Target1;
};

Out main(float2 uv : TEXCOORD0)
{
    uint w;
    uint h;
    GlowMap.GetDimensions(w, h);
    int2 texel = int2(uv * float2(w, h));
    float4 ld = GlowMap.Load(int3(texel, 0), int2(1, 1));
    float4 sm = GlowMap.Sample(Samp, uv);
    float4 g = (ld.a == 0.0) ? sm : ld;
    float4 prm = Params[0];
    float3 hsv = rgb2hsv(g.rgb);
    hsv.x = frac(hsv.x + prm.x);
    hsv.y = max(hsv.y + prm.y, 0.0);
    hsv.z = max(hsv.z + prm.z, 0.0);
    float3 rgb = hsv2rgb(hsv);
    float on = ((w + h) != 0 && g.a != 0.0) ? 1.0 : 0.0;
    Out o;
    o.o0 = float4(rgb * g.a * prm.w, on);
    o.o3 = float4(rgb, on);
    return o;
}
