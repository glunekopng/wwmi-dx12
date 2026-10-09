#pragma once
// RABBITFX TEXTURE BINDING -- the mod's diffuse/normal/light into an injected draw's PS slots.
#include <cstdint>
#include <vector>
#include <utility>
#include <reshade.hpp>
#include <d3d12.h>

struct RfxBindState {
    bool                                      bound = false;
    bool                                      applied = false;
    std::vector<std::pair<uint8_t, uint64_t>> tables;
    ID3D12DescriptorHeap*                     game_srv = nullptr;
    ID3D12DescriptorHeap*                     game_smp = nullptr;
};
struct WclSlotOvr {
    int                         reg;
    D3D12_CPU_DESCRIPTOR_HANDLE srv;
    bool                        is_null;
};
struct ShadowSlotOvr {
    uint8_t                     param;
    uint32_t                    offset;
    D3D12_CPU_DESCRIPTOR_HANDLE src;
};
void SetShadowExtraSlots(const ShadowSlotOvr* s, unsigned n);

RfxBindState ApplyDrawBindings(reshade::api::command_list* cmd_list, ID3D12GraphicsCommandList* cl,
    const std::vector<WclSlotOvr>& binds, bool guard_bindless);
struct RfxDumpBind {
    int                         reg;
    D3D12_CPU_DESCRIPTOR_HANDLE srv;
    ID3D12Resource*             res;
}; // res: the mod texture behind srv, or nullptr
const std::vector<RfxDumpBind>*        RfxBindsForDump(unsigned draw_index);
bool                                   RfxParam0Covers(reshade::api::command_list* cmd_list, unsigned max_reg, unsigned* base_reg, unsigned* count);
extern std::atomic<unsigned long long> g_rfx_mirror_misses;
bool                                   RfxPatchNullSrvs(D3D12_CPU_DESCRIPTOR_HANDLE* null_tex, D3D12_CPU_DESCRIPTOR_HANDLE* null_buf);
D3D12_CPU_DESCRIPTOR_HANDLE            RfxPatchParamsSrv(float hue, float sat, float val, float brightness);
RfxBindState                           RfxDecideAndBind(reshade::api::command_list* cmd_list, ID3D12GraphicsCommandList* cl,
    int mod_idx, int comp_idx, D3D12_CPU_DESCRIPTOR_HANDLE dif,
    D3D12_CPU_DESCRIPTOR_HANDLE nor, D3D12_CPU_DESCRIPTOR_HANDLE lit,
    D3D12_CPU_DESCRIPTOR_HANDLE mat, bool gbuffer_pass, uint8_t ps_reads, const std::vector<WclSlotOvr>* extra = nullptr);
void                                   RfxRestore(ID3D12GraphicsCommandList* cl, const RfxBindState& st);
