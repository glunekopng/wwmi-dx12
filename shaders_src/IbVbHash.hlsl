// IbVbHash.hlsl -- stateless WWMI character recognition by the POSITION VB hash (3dmigoto's ini `hash=`).
// One dispatch of ONE group of 1024 threads (runs once per (ib, vb) pair, cached, deferred):
//   pass 1: max vertex index over the IB, each thread a strided subset, InterlockedMax into groupshared
//           -> vertexCount = max+1 (the vanilla vertex count, which the ini does not carry)
//   pass 2: crc32c of the VB prefix [0 : vertexCount*stride] in 1024 contiguous 4-byte-aligned slices,
//           one per thread, in place through the SRV; every thread writes (crc, sliceBytes)
// The CPU combines the 1024 slice CRCs (zlib's crc32_combine, Castagnoli polynomial) and chains the
// D3D11_BUFFER_DESC: crc32c_std(crc, {vbBytes, Usage=0, Bind=0x9 (VERTEX|SHADER_RESOURCE), 0,0,0}).
//
// PARALLEL: one thread over a 4.5 MB prefix holds the GPU for over a second (a TDR risk); 1024 slices of
// it are 4.4 KB each.
//
// CLAMPED to the VB view's extent (g_vbExtent), so the CPU never chains a ByteWidth the buffer does not have.
//
// Out layout: [0] vbBytes (clamped)  [4] maxIdx  then per thread t at 8 + t*8: (crc32c of its slice, bytes).

ByteAddressBuffer   IB  : register(t0);   // game index buffer  [base .. base+ibBytes]
ByteAddressBuffer   VB  : register(t1);   // game position VB   [base .. base+vbExtent]
RWByteAddressBuffer Out : register(u0);

cbuffer Params : register(b0) {
    uint g_ibBytes;
    uint g_idxSize;
    uint g_vbStride;
    uint g_vbExtent;
};

#define THREADS 1024u

groupshared uint gMax;

uint crc_byte(uint crc, uint b)
{
    crc ^= (b & 0xFFu);
    [unroll] for (int i = 0; i < 8; ++i)
        crc = (crc >> 1) ^ (0x82F63B78u & (uint)(-(int)(crc & 1u)));
    return crc;
}

[numthreads(1024, 1, 1)]
void main(uint tid : SV_GroupIndex)
{
    if (tid == 0u) gMax = 0u;
    GroupMemoryBarrierWithGroupSync();

    // pass 1: max index over the IB, strided
    const uint nIdx = g_ibBytes / g_idxSize;
    uint localMax = 0u;
    for (uint i = tid; i < nIdx; i += THREADS) {
        uint v;
        if (g_idxSize == 2u) {
            uint b = i * 2u;
            uint w = IB.Load(b & ~3u);
            v = (b & 2u) ? (w >> 16) : (w & 0xFFFFu);
        }
        else                 { v = IB.Load(i * 4u); }
        localMax = max(localMax, v);
    }
    InterlockedMax(gMax, localMax);
    GroupMemoryBarrierWithGroupSync();

    const uint maxIdx  = gMax;
    uint vbBytes = (maxIdx + 1u) * g_vbStride;
    vbBytes = min(vbBytes, g_vbExtent);

    // pass 2: this thread's slice of the prefix, 4-byte aligned so every Load is aligned
    const uint slice = (((vbBytes + THREADS - 1u) / THREADS) + 3u) & ~3u;
    const uint lo = min(tid * slice, vbBytes);
    const uint hi = min(lo + slice, vbBytes);
    uint crc = 0xFFFFFFFFu;
    uint j = lo;
    for (; j + 4u <= hi; j += 4u) {
        uint w = VB.Load(j);
        crc = crc_byte(crc, w);
        crc = crc_byte(crc, w >> 8);
        crc = crc_byte(crc, w >> 16);
        crc = crc_byte(crc, w >> 24);
    }
    if (j < hi) {
        uint w = VB.Load(j);
        uint s = 0u;
        for (; j < hi; ++j, s += 8u) crc = crc_byte(crc, w >> s);
    }

    Out.Store2(8u + tid * 8u, uint2(crc ^ 0xFFFFFFFFu, hi - lo));
    if (tid == 0u) Out.Store2(0u, uint2(vbBytes, maxIdx));
}
