// BufCopy -- a buffer-to-buffer copy that stays in the compute pipe. The mod's per-component `copy` lines
// move a few KB each; CopyResource on a direct list hands that to the copy engine and waits for it, ~65 us
// a copy, 23 copies a frame. One thread moves 16 bytes.
cbuffer C : register(b0)
{
    uint src_off;
    uint dst_off;
    uint bytes;
    uint pad;
}
ByteAddressBuffer   src : register(t0);
RWByteAddressBuffer dst : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint o = id.x * 16;
    if (o >= bytes) return;
    if (o + 16 <= bytes) {
        dst.Store4(dst_off + o, src.Load4(src_off + o));
        return;
    }
    for (uint k = o; k < bytes; k += 4)
        dst.Store(dst_off + k, src.Load(src_off + k));
}
