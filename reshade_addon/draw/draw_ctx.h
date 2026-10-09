#pragma once
#include "../addon_common.h"
#include "../mods/layout_registry.h" // g_layout_b4_root / g_layout_b3_root
#include "../mods/skeleton.h"        // MergedComp

struct DrawArgs {
    reshade::api::command_list* cmd_list;
    ID3D12GraphicsCommandList*  d3d12_cl;
    uint32_t                    index_count;
    uint32_t                    instance_count;
    uint32_t                    first_index;
    int32_t                     vertex_offset;
    uint32_t                    first_instance;
    uint64_t                    vs_hash;
    CmdRT                       draw_rt; // stable for this draw: only the game's bind hook writes it
};

// Set by the claim (draw_claim.cpp) and the depth-pass recognition (draw_shadow.cpp); read by the inject path.
struct DrawClaim {
    MergedComp*     mc = nullptr;          // the component this draw is, when claimed
    MergedComp*     depth_claim = nullptr; // the claim a DEPTH draw earned and then released -- the shadow pass's identity
    bool            mc_cb13 = false;       // did the game bind a CBV at the b4 root for this draw?
    uint64_t        cb13_va = 0;           // the game's current (b4) / previous (b3) skeleton CBVs
    uint64_t        cb14_va = 0;
    ID3D12Resource* port_cb4_res = nullptr;
    uint64_t        port_cb4_va = 0;
    ID3D12Resource* port_cb3_res = nullptr;
    uint64_t        port_cb3_va = 0;
    int             cb13_root = -1; // b4 (current) / b3 (previous) root params for this pass
    int             cb14_root = -1;
    uint64_t        rep_vs = 0;          // VS hash whose mod.ini rules are applied
    bool            full_shadow = false; // a recognised character depth pass: the mod components are cast into it
    uint64_t        shadow_dsv = 0;      // which shadow map it casts into
    uint32_t        shadow_pass = 0;     // render-pass instance (CmdRT::pass_gen) -- distinguishes reuse of one target
};

struct BoneRoots {
    reshade::api::command_list* cl;
    bool                        done = false;
    int                         b4 = -1;
    int                         b3 = -1;
    RootCbvBind                 b4cb = {};
    RootCbvBind                 b3cb = {};
    const BoneRoots&            Get()
    {
        if (done) return *this;
        done = true;
        const uint64_t lay = CmdGfxLayout(cl);
        {
            std::lock_guard ll(g_layout_mutex);
            auto            a = g_layout_b4_root.find(lay);
            if (a != g_layout_b4_root.end()) b4 = a->second;
            auto b = g_layout_b3_root.find(lay);
            if (b != g_layout_b3_root.end()) b3 = b->second;
        }
        const CmdCbvBinds& cb = CmdCbv(cl);
        if (b4 >= 0 && b4 < 64) b4cb = cb.binds[b4];
        if (b3 >= 0 && b3 < 64) b3cb = cb.binds[b3];
        return *this;
    }
};
