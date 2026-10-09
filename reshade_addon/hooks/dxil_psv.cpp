// dxil_psv.cpp -- see dxil_psv.h.
//
// Container: 'DXBC', 16-byte digest, u32 version, u32 total size, u32 part count, u32 offsets[count];
// each part: fourcc, u32 size, data. PSV0 (DxilPipelineStateValidation.h, dxc):
//   u32 PSVRuntimeInfo_size; PSVRuntimeInfo (that many bytes; its version is implied by the size);
//   u32 ResourceCount; if ResourceCount: u32 PSVResourceBindInfo_size; ResourceCount records of that size:
//     PSVResourceBindInfo0 { u32 ResType; u32 Space; u32 LowerBound; u32 UpperBound; }        (16 bytes)
//     PSVResourceBindInfo1 adds { u32 ResKind; u32 ResFlags; }                                   (24 bytes)
//   (string table, semantic index table and signature elements follow in v1+; not needed.)
// PSVResourceType: 0 Invalid, 1 Sampler, 2 CBV, 3 SRVTyped, 4 SRVRaw, 5 SRVStructured, 6..9 UAVs.
// UpperBound is 0xFFFFFFFF for an unbounded range. Everything is read through bounds-checked u32 loads:
// a truncated or foreign blob yields `parsed = false`, never a read past the end.
#include "dxil_psv.h"
#include <cstring>

namespace {
bool u32_at(const uint8_t* p, size_t size, size_t off, uint32_t* out)
{
    if (off + 4 > size) return false;
    memcpy(out, p + off, 4);
    return true;
}
}

bool DxilShaderSrvRegs(const void* code, size_t size, DxilSrvRegs& out)
{
    out = DxilSrvRegs{};
    const uint8_t* p = static_cast<const uint8_t*>(code);
    if (!p || size < 32 || memcmp(p, "DXBC", 4) != 0) return false;
    uint32_t nparts = 0;
    if (!u32_at(p, size, 28, &nparts) || nparts > 64) return false;
    for (uint32_t i = 0; i < nparts; ++i) {
        uint32_t off = 0;
        uint32_t psize = 0;
        if (!u32_at(p, size, 32 + (size_t)i * 4, &off)) return false;
        if (off + 8 > size || memcmp(p + off, "PSV0", 4) != 0) continue;
        if (!u32_at(p, size, off + 4, &psize) || off + 8 + (size_t)psize > size) return false;
        const uint8_t* d = p + off + 8;
        const size_t   n = psize;
        uint32_t       info_size = 0;
        uint32_t       res_count = 0;
        uint32_t       rec_size = 0;
        size_t         pos = 0;
        if (!u32_at(d, n, pos, &info_size)) return false;
        pos += 4 + info_size;
        if (!u32_at(d, n, pos, &res_count)) return false;
        pos += 4;
        if (res_count == 0) {
            out.parsed = true;
            return true;
        }
        if (!u32_at(d, n, pos, &rec_size) || rec_size < 16 || res_count > 4096) return false;
        pos += 4;
        if (pos + (size_t)res_count * rec_size > n) return false;
        for (uint32_t r = 0; r < res_count; ++r, pos += rec_size) {
            uint32_t type = 0;
            uint32_t space = 0;
            uint32_t lo = 0;
            uint32_t hi = 0;
            uint32_t kind = 0;
            u32_at(d, n, pos + 0, &type);
            u32_at(d, n, pos + 4, &space);
            u32_at(d, n, pos + 8, &lo);
            u32_at(d, n, pos + 12, &hi);
            if (rec_size >= 24) u32_at(d, n, pos + 16, &kind);
            if (type < 3 || type > 5 || space != 0) continue; // SRVs of space 0 only
            if (hi == 0xFFFFFFFFu || hi < lo) {
                out.unbounded = true;
                continue;
            }
            for (uint32_t t = lo; t <= hi && t < 128; ++t) {
                if (t < 64)
                    out.lo |= (1ull << t);
                else
                    out.hi |= (1ull << (t - 64));
                out.kind[t] = (uint8_t)(kind > 255 ? 0 : kind);
            }
        }
        out.parsed = true;
        return true;
    }
    return false;
}
