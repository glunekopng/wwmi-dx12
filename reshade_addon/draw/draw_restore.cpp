// draw_restore.cpp -- the game's bindings put back after an injected draw. See draw_inject.h.
#include "../addon_common.h"
#include "../hooks/cmd_bind_hooks.h"
#include "../migoto/dx12_bridge.h"
#include "../instrumentation/gpu_timer.h"
#include "draw_inject.h"

void RestoreGameState(InjectFrame& fr)
{
    reshade::api::command_list* const cmd_list = fr.da.cmd_list;
    ID3D12GraphicsCommandList* const  d3d12_cl = fr.da.d3d12_cl;
    const uint64_t                    game_cb13_va = fr.dc.cb13_va;
    const uint64_t                    game_cb14_va = fr.dc.cb14_va;
    const int                         game_cb13_root = fr.dc.cb13_root;
    const int                         game_cb14_root = fr.dc.cb14_root;
    D3D12_VERTEX_BUFFER_VIEW(&save_vb)
    [8] = fr.save_vb;
    bool (&save_vb_set)[8] = fr.save_vb_set;
    const unsigned reloc_vb_mask = fr.reloc_vb_mask;
    RelocXition(&reloc_xit)[9] = fr.reloc_xit;
    unsigned&                reloc_xit_n = fr.reloc_xit_n;
    D3D12_INDEX_BUFFER_VIEW& save_ib = fr.save_ib;
    const bool               save_ib_set = fr.save_ib_set;
    const bool               cb_ok = fr.cb_ok;
    const bool               x32_mid = fr.x32_mid;
    unsigned&                x32_mid_restores = fr.x32_mid_restores;
    unsigned&                x32_mid_skipped = fr.x32_mid_skipped;
    PROF(PROF_DI_Restore); // runs on EVERY claimed draw: VBs + IB + 2 CBVs + non-PSO state
    const bool x32_skip_ia = x32_mid;
    if (x32_mid) {
        ++x32_mid_restores;
        if (x32_skip_ia) ++x32_mid_skipped;
    }
    if (!x32_skip_ia) {
        if (reloc_xit_n) {
            GpuBracket _gt(d3d12_cl, GT_RelocRestore);
            for (unsigned x = 0; x < reloc_xit_n; ++x)
                MigotoPort::OwnedTransition(d3d12_cl, reloc_xit[x].res, reloc_xit[x].back);
        }
        reloc_xit_n = 0;
        if (!g_restore_vbib && reloc_vb_mask) {
            for (int s = 0; s < 8; ++s)
                if ((reloc_vb_mask & (1u << s)) && save_vb_set[s])
                    d3d12_cl->IASetVertexBuffers(s, 1, &save_vb[s]);
            if (save_ib_set) d3d12_cl->IASetIndexBuffer(&save_ib);
        }
        if (g_restore_vbib) {
            for (int s = 0; s < 5; ++s) {
                if (save_vb_set[s])
                    d3d12_cl->IASetVertexBuffers(s, 1, &save_vb[s]);
                else {
                    D3D12_VERTEX_BUFFER_VIEW nullv = { 0, 0, 0 };
                    d3d12_cl->IASetVertexBuffers(s, 1, &nullv);
                }
            }
            for (int s = 5; s < 8; ++s)
                if (reloc_vb_mask & (1u << s))
                    d3d12_cl->IASetVertexBuffers(s, 1, &save_vb[s]);
            if (save_ib_set) d3d12_cl->IASetIndexBuffer(&save_ib);
        }
    } // !x32_skip_ia -- X32: a mid-section call leaves the IA state and the
    // relocation transitions to the hook's own end-of-draw restore.
    if (cb_ok && g_restore_cb) {
        if (game_cb13_va && game_cb13_root >= 0 && game_cb13_root < 64)
            d3d12_cl->SetGraphicsRootConstantBufferView((UINT)game_cb13_root, game_cb13_va);
        if (game_cb14_root >= 0 && game_cb14_va)
            d3d12_cl->SetGraphicsRootConstantBufferView((UINT)game_cb14_root, game_cb14_va);
    }
    if (g_restore_nonpso) {
        auto& gs = CmdGfx(cmd_list);
        if (MigotoPort::g_touched_viewport) {
            if (gs.vp_count) d3d12_cl->RSSetViewports(gs.vp_count, gs.vp);
            if (gs.scissor_count) d3d12_cl->RSSetScissorRects(gs.scissor_count, gs.scissor);
            MigotoPort::g_touched_viewport = false;
        }
        if (gs.topology) d3d12_cl->IASetPrimitiveTopology((D3D12_PRIMITIVE_TOPOLOGY)gs.topology);
        if (MigotoPort::g_touched_blend && gs.blend_factor_set)
            d3d12_cl->OMSetBlendFactor(gs.blend_factor);
        if (MigotoPort::g_touched_stencil && gs.stencil_ref_set)
            d3d12_cl->OMSetStencilRef(gs.stencil_ref);
        // Cleared even when we could not restore, so a set flag never leaks into the next draw.
        MigotoPort::g_touched_blend = MigotoPort::g_touched_stencil = false;
        if (!gs.topology || !gs.blend_factor_set || !gs.stencil_ref_set) {
            static std::atomic<bool> once{ false };
            bool                     e = false;
            if (once.compare_exchange_strong(e, true))
                logger::write("RESTORE-GAP: injected draw could not restore non-PSO state never "
                              "observed from the game (topology=%d blend=%d stencil=%d) -- it "
                              "stays as our draw left it",
                    (int)(gs.topology != 0), (int)gs.blend_factor_set,
                    (int)gs.stencil_ref_set);
        }
    }
}
