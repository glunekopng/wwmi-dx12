#pragma once
#include <cstdint>
#include <cstddef>

struct DxilSrvRegs {
    uint64_t lo = 0; // t0..t63 / t64..t127 bound in space 0 (SRV of any kind)
    uint64_t hi = 0;
    bool     unbounded = false; // an SRV range with no upper bound (bindless): every register may be read
    uint8_t  kind[128] = {};    // DXIL ResourceKind per register (0 = unknown / PSV0 v0)
    bool     parsed = false;    // a PSV0 part was found and read; false = fall back to the root signature
    bool     has(unsigned r) const { return r < 64 ? ((lo >> r) & 1u) != 0 : (r < 128 && ((hi >> (r - 64)) & 1u) != 0); }
    // May this register be read at all: declared, or reachable through an unbounded range.
    bool reads(unsigned r) const { return !parsed || unbounded || has(r); }
};

bool DxilShaderSrvRegs(const void* code, size_t size, DxilSrvRegs& out);
