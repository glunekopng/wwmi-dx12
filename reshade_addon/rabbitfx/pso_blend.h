#pragma once
#include <reshade.hpp>
#include <d3d12.h>
#include <string>

void PsoCloneStore(uint64_t pso_handle, reshade::api::pipeline_layout layout,
    uint32_t subobject_count, const reshade::api::pipeline_subobject* subobjects);

void PsoCloneForget(reshade::api::device* dev, uint64_t pso_handle);

struct PsoCloneStats {
    size_t   clones;
    size_t   bytes;
    size_t   code_blobs;
    size_t   code_bytes;
    unsigned forgotten;
    unsigned variants_released;
};
PsoCloneStats PsoCloneGetStats();

unsigned PsoCloneDescribeDepthStencil(uint64_t pso_handle, char* out, size_t cap);
unsigned PsoCloneWriteShaders(uint64_t pso_handle, const wchar_t* dir);

ID3D12PipelineState* GetBlendVariant(reshade::api::device* dev, uint64_t src_pso,
    const std::string& blend_mode, const std::string& alpha_mode, const float bf[4], bool decensor);

ID3D12PipelineState* GetBlendVariantFromDesc(reshade::api::device* dev, uint64_t src_pso,
    const struct D3D11_BLEND_DESC* bdesc, const float bf[4], bool decensor);

constexpr unsigned   kRfxGlowReg = 50; // ps-t50 / ps-t51, the ini's registers
constexpr unsigned   kRfxCutoutReg = 51;
constexpr unsigned   kRfxParamsReg = 52; // ours: the ini's t120 is outside the game's t0..t63 table
ID3D12PipelineState* GetRfxPatchVariant(reshade::api::device* dev, uint64_t src_pso, unsigned* ps_max_srv_reg, bool* has_glow, bool decensor);
void                 RfxPatchWorkerStop();
void                 RfxDecensorQueue(reshade::api::device* dev, uint64_t src_pso); // a decensor-only variant of a game PSO; settles into PsoSetDecensorVariant
struct RfxPatchStats {
    unsigned ps_patched;
    unsigned ps_failed;
    unsigned variants;
}; // per game instance (the disk cache refills them)
RfxPatchStats GetRfxPatchStats();
