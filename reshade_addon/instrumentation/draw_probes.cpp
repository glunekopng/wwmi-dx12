// draw_probes.cpp -- the draw path's debug-only lines (g_debug, most armed by ProbeArm). Nothing here changes
// what is drawn. See draw_parts.h.
#include "../addon_common.h"
#include "removal_pin.h"
#include "../mods/claim_state.h"
#include "../hooks/resource_tracker.h"
#include "../mods/layout_registry.h"
#include "../draw/draw_ctx.h"
#include "../draw/draw_parts.h"
#include <set>

using namespace reshade::api;

void DrawProbesClaim(const DrawArgs& da, DrawClaim& dc)
{
    reshade::api::command_list* const cmd_list = da.cmd_list;
    const uint32_t                    index_count = da.index_count;
    const uint32_t                    first_index = da.first_index;
    const uint64_t                    vs_hash = da.vs_hash;
    MergedComp*&                      mc = dc.mc;
    if (g_debug) {
        auto it = g_first_to_comp.find(FirstCountKey(first_index, index_count));
        if (it != g_first_to_comp.end()) {
            const uint64_t k = ((uint64_t)first_index << 24) ^ index_count;
            if (ProbeArmed() && DiagOnce("DRAW-FIRSTCOUNT", (int)(k ^ (k >> 32)))) {
                uint64_t vb0dbg = 0;
                {
                    vb0dbg = CmdGfx(cmd_list).vb[0].va;
                }
                logger::trace("CLAIMHIT comp first=%u count=%u vs=%016llx vb0=%llx claimed=%d",
                    first_index, index_count, vs_hash, vb0dbg, (int)(mc != nullptr));
                {
                    int             md2 = g_merged_comps[it->second].mod_id;
                    const uint32_t  maxEnd = ModIndexEnd(md2);
                    ID3D12Resource* ibr = nullptr;
                    ID3D12Resource* vbr = nullptr;
                    uint64_t        ibva = 0;
                    uint64_t        vbva = 0;
                    uint32_t        idxb = 2;
                    uint32_t        vstride = 0;
                    {
                        auto& st = CmdGfx(cmd_list);
                        ibr = st.ib.res;
                        ibva = st.ib.va;
                        if (st.ib.index_bytes) idxb = st.ib.index_bytes;
                        vbr = st.vb[0].res;
                        vbva = st.vb[0].va;
                        vstride = st.vb[0].stride;
                    }
                    if (ibr && vbr && maxEnd && vstride) {
                        uint32_t                  iboff = (uint32_t)(ibva - ibr->GetGPUVirtualAddress());
                        uint32_t                  vboff = (uint32_t)(vbva - vbr->GetGPUVirtualAddress());
                        uint32_t                  got = IbHash::Lookup(ibr, iboff, maxEnd * idxb, idxb, vbr, vboff, vstride);
                        const WwmiMod::Component* wc2 = WmodComp(md2, g_merged_comps[it->second].comp_idx);
                        logger::trace("IBHASH-ANCHOR mod=%d ibBytes=%u vstride=%u ourVbHash=%08x iniHash=%08x %s",
                            md2, maxEnd * idxb, vstride, got, wc2 ? wc2->ib_hash : 0,
                            (got && wc2 && got == wc2->ib_hash) ? "*** MATCH ***" : (got ? "nomatch" : "pending"));
                    }
                }
            }
        }
    }

    if (g_debug && mc) {
        uint64_t ps_hash = 0;
        cmd_list->get_private_data(s_iid_ps_hash, &ps_hash);
        const uint64_t k = vs_hash ^ (ps_hash * 1099511628211ULL);
        if (ProbeArmed() && DiagOnce("CLAIMVSPS", (int)(k ^ (k >> 32)))) logger::trace("CLAIMVSPS comp=%d vs=%016llx ps=%016llx first=%u count=%u",
            mc->comp_idx, vs_hash, ps_hash, first_index, index_count);
    }
}

void DrawProbesPasses(const DrawArgs& da, BoneRoots& roots, DrawClaim& dc)
{
    reshade::api::command_list* const cmd_list = da.cmd_list;
    const uint32_t                    index_count = da.index_count;
    const uint32_t                    first_index = da.first_index;
    const uint64_t                    vs_hash = da.vs_hash;
    const CmdRT&                      draw_rt = da.draw_rt;
    MergedComp*&                      mc = dc.mc;
    if (g_debug && index_count >= 64 && ProbeArmed()) {
        const uint64_t    vb0 = CmdGfx(cmd_list).vb[0].va;
        const int         b4r = roots.Get().b4;
        const RootCbvBind b4cb = roots.Get().b4cb;
        bool              charish_vb = (vb0 >= 0x300000000ULL && vb0 < 0x400000000ULL);
        {
            std::lock_guard l(g_char_mesh_mtx);
            if (!charish_vb && vb0 && g_char_mesh_vb0.count(vb0)) charish_vb = true;
        }
        uint32_t sh = HashBoneCB(b4cb.res, b4cb.va);
        bool     inskel = false;
        {
            std::lock_guard l(g_char_skel_mtx);
            inskel = sh && g_frame_skel.count(sh) != 0;
        }
        if (charish_vb || inskel) { // character draws only -- no world depth flood
            uint64_t pprobe = 0;
            cmd_list->get_private_data(s_iid_ps_hash, &pprobe);
            const CmdRT&   rt = draw_rt;
            const uint64_t k = vs_hash ^ (pprobe * 1099511628211ULL) ^ ((uint64_t)first_index << 1) ^ ((uint64_t)index_count << 20);
            const bool     fresh = ProbeArmed() && DiagOnce("DEPTHPASS-PROBE", (int)(k ^ (k >> 32)));
            // rtv=0 + dsv=1 => DEPTH-ONLY pass (shadow caster / prepass) = the pass we're hunting.
            if (fresh) logger::trace("SHADOWPROBE vs=%016llx ps=%016llx first=%u count=%u vb0=%llx b4r=%d claimed=%d rtv=%u dsv=%d charVB=%d inSkel=%d",
                vs_hash, pprobe, first_index, index_count, vb0, b4r, (int)(mc != nullptr), rt.rtv_count, (int)rt.has_dsv, (int)charish_vb, (int)inskel);
            if (fresh && rt.rtv_count == 0 && rt.has_dsv) {
                ID3D12Resource* pres[64];
                uint64_t        pva[64];
                int             pp[64];
                int             npv = 0;
                {
                    auto it = &CmdCbv(cmd_list);
                    if (it != nullptr)
                        for (int p = 0; p < 64; ++p) {
                            auto& b = it->binds[p];
                            if (b.res && b.va) {
                                pp[npv] = p;
                                pres[npv] = b.res;
                                pva[npv] = b.va;
                                ++npv;
                            }
                        }
                }
                char cbs[460];
                int  n = 0;
                cbs[0] = 0;
                for (int pi = 0; pi < npv && n < 410; ++pi) {
                    uint32_t hp = HashBoneCB(pres[pi], pva[pi]);
                    bool     ins = false;
                    {
                        std::lock_guard l(g_char_skel_mtx);
                        ins = hp && g_frame_skel.count(hp) != 0;
                    }
                    n += snprintf(cbs + n, sizeof(cbs) - n, "b%d=%llx%s ", pp[pi], (unsigned long long)pva[pi],
                        ins ? "*SKEL*" : "");
                }
                logger::write("SHADOWCBV vs=%016llx first=%u b4r=%d binds:[%s]", vs_hash, first_index, b4r, cbs);
            }
        }
    }


    if (g_debug) {
        PROF(PROF_DI_Probe);
        static std::mutex         s_pd;
        static std::set<uint64_t> s_char;
        static std::set<uint64_t> s_other;
        // On-demand RE-ARM: when the overlay bumps g_diag_gen, wipe this capture so a fresh one starts HERE.
        static uint32_t s_gen = 0;
        {
            uint32_t g = g_diag_gen.load();
            if (g != s_gen) {
                std::lock_guard l(s_pd);
                s_char.clear();
                s_other.clear();
                s_gen = g;
            }
        }
        // ARM ONLY AFTER A MOD CHARACTER APPEARS (pool is shared by all actors -- see SKINPROBE): require its mesh known.
        bool charUp;
        {
            std::lock_guard l(g_char_mesh_mtx);
            charUp = !g_char_mesh_vb0.empty();
        }
        if (charUp && index_count >= 32) {
            const uint64_t    vb0 = CmdGfx(cmd_list).vb[0].va;
            const RootCbvBind b4cb = roots.Get().b4cb;
            bool              boneNear = false;
            uint64_t          bdist = ~0ull;
            if (b4cb.va) {
                std::lock_guard l(g_skincache_mtx);
                for (uint64_t v : g_char_bone_va) {
                    uint64_t d = b4cb.va > v ? b4cb.va - v : v - b4cb.va;
                    if (d < bdist) bdist = d;
                }
                boneNear = (bdist < 0x80000ULL);
            }
            bool inPool = InSkinBand(vb0);
            if (boneNear || inPool) {
                uint64_t k = vs_hash ^ ((uint64_t)first_index << 3) ^ ((uint64_t)index_count << 20);
                bool     fresh = false;
                {
                    std::lock_guard l(s_pd);
                    if (boneNear) {
                        if (s_char.size() < 96) fresh = s_char.insert(k).second;
                    } else {
                        if (s_other.size() < 24) fresh = s_other.insert(k).second;
                    }
                }
                if (fresh) {
                    uint64_t psh = 0;
                    cmd_list->get_private_data(s_iid_ps_hash, &psh);
                    uint32_t bh = HashBoneCB(b4cb.res, b4cb.va);
                    bool     inSkel = false;
                    {
                        std::lock_guard l(g_char_skel_mtx);
                        inSkel = bh && g_frame_skel.count(bh) != 0;
                    }
                    // rtv/dsv: rtv=0 + dsv=1 => DEPTH/SHADOW caster; rtv>0 => VISIBLE pass.
                    const CmdRT& rt = draw_rt;
                    logger::trace("POOLDRAW %s vs=%016llx ps=%016llx first=%u count=%u vb0=%llx claimed=%d rtv=%u dsv=%d b4va=%llx boneNear=%d bdist=%llx inSkel=%d",
                        boneNear ? "CHAR " : "world", vs_hash, psh, first_index, index_count, vb0, (int)(mc != nullptr), rt.rtv_count, (int)rt.has_dsv, b4cb.va, (int)boneNear, bdist, (int)inSkel);
                }
            }
        }
    }

    if (g_debug && index_count >= 32) {
        uint64_t vb0 = 0;
        {
            vb0 = CmdGfx(cmd_list).vb[0].va;
        }
        bool skinlod = false;
        if (vb0) {
            std::lock_guard l(g_skincache_mtx);
            for (auto& o : g_skincache_out)
                if (vb0 >= o.va && vb0 < o.va + o.size) {
                    skinlod = true;
                    break;
                }
        }
        if (skinlod) {
            const uint64_t k = vs_hash ^ ((uint64_t)first_index << 1) ^ ((uint64_t)index_count << 20);
            if (ProbeArmed() && DiagOnce("DRAW-VSRANGE", (int)(k ^ (k >> 32)))) {
                uint64_t psh = 0;
                cmd_list->get_private_data(s_iid_ps_hash, &psh);
                logger::write("SKINLOD vs=%016llx ps=%016llx first=%u count=%u vb0=%llx claimed=%d (char via skin-cache lineage)",
                    vs_hash, psh, first_index, index_count, vb0, (int)(mc != nullptr));
            }
        }
    }
}

void DrawProbeVb6Reader(const DrawArgs& da, DrawClaim& dc)
{
    reshade::api::command_list* const cmd_list = da.cmd_list;
    ID3D12GraphicsCommandList* const  d3d12_cl = da.d3d12_cl;
    const uint32_t                    index_count = da.index_count;
    const uint32_t                    instance_count = da.instance_count;
    const uint32_t                    first_index = da.first_index;
    const uint64_t                    vs_hash = da.vs_hash;
    const CmdRT&                      draw_rt = da.draw_rt;
    MergedComp*&                      mc = dc.mc;
    if (g_debug) {
        PROF(PROF_DI_Vb6Reader);
        ID3D12Resource* r6 = nullptr;
        uint32_t        s6 = 0;
        uint64_t        va0 = 0;
        const CmdRT&    rt6 = draw_rt;
        {
            const auto& cg = CmdGfx(cmd_list);
            r6 = cg.vb[6].res;
            s6 = cg.vb[6].stride;
            va0 = cg.vb[0].va;
        }
        RelocBuf rb6{};
        if (r6 && s6 && g_buffer_reloc.Resolve(r6, &rb6)) {
            if (mc)
                g_vb6_readers_claimed.fetch_add(1, std::memory_order_relaxed);
            else {
                g_vb6_readers_unclaimed.fetch_add(1, std::memory_order_relaxed);
                RemovalPin::Mark(d3d12_cl, "game: unclaimed draw reading a relocated morph buffer"); // DRED context
                const int key = (int)(((vs_hash ^ ((uint64_t)first_index * 0x9E3779B97F4A7C15ull) ^ (uint64_t)index_count) >> 1) & 0x7fffffff);
                if (DiagOnce((const void*)&g_vb6_readers_unclaimed, key)) {
                    const unsigned ob = g_buffer_reloc.IdentityBytes(rb6.res, 0);
                    const bool     in_pool = InSkinBand(va0);
                    logger::write("UNCLAIMED-VB6-READER vs=%016llx first=%u count=%u inst=%u rtv=%u dsv=%d "
                                  "vb0=%llx inPool=%d slot6=%p (%u B vanilla, widened to %llu B): a GAME draw "
                                  "reading the vanilla morph buffer -- vanilla geometry, drawn as-is",
                        (unsigned long long)vs_hash, first_index, index_count, instance_count,
                        rt6.rtv_count, (int)rt6.has_dsv, (unsigned long long)va0, (int)in_pool,
                        (void*)r6, ob, (unsigned long long)rb6.bytes);
                }
            }
        }
    }
}

void DrawProbeOffLeak(const DrawArgs& da, DrawClaim& dc)
{
    if (!g_debug || dc.mc || !TexOffWatching() || !ProbeArmed()) return;
    reshade::api::command_list* const cmd_list = da.cmd_list;
    const auto&                       root = CmdGfx(cmd_list).root;
    uint32_t                          scan_len[64];
    LayoutTexScanLens(CmdGfxLayout(cmd_list), scan_len);
    for (int p = 0; p < 64; ++p) {
        if (root[p].table.handle == 0) continue;
        const TableStart ts = TableStartOf(cmd_list->get_device(), root[p].table);
        if (!ts) continue;
        const uint32_t num = ts.Heap()->GetDesc().NumDescriptors;
        for (uint32_t s = 0; s < scan_len[p] && ts.base + s < num; ++s) {
            ResInfo ri;
            if (!g_heap_to_res.find({ ts.heap, ts.base + s }, ri) || !ri.res || !ri.swapped) continue;
            const uint32_t rule = TexOffRuleOf(ri.res);
            if (!rule) continue;
            g_off_leaks.fetch_add(1, std::memory_order_relaxed);
            uint64_t ps = 0;
            cmd_list->get_private_data(s_iid_ps_hash, &ps);
            const uint64_t k = da.vs_hash ^ (ps * 0x9E3779B97F4A7C15ull) ^ rule;
            if (DiagOnce("OFFLEAK", (int)((k ^ (k >> 32)) & 0x7fffffff)))
                logger::write("OFFLEAK f=%u vs=%016llx ps=%016llx first=%u count=%u rtv=%u p%d t%u: rule %08x res=%p "
                              "still stamped with %p (slot written %u frame(s) ago, src %u) -- a draw we do not hand back "
                              "reads the switched-off mod's texture",
                    g_frame_id, (unsigned long long)da.vs_hash, (unsigned long long)ps, da.first_index,
                    da.index_count, da.draw_rt.rtv_count, p, s, rule, (void*)ri.res, (void*)ri.swapped,
                    g_frame_id - ri.wrote_frame, (unsigned)ri.wrote_src);
        }
    }
}

void DrawProbeUnstamp(const ResInfo& ri, bool restored)
{
    if (!g_debug || !TexOffWatching()) return;
    const uint32_t rule = TexOffRuleOf(ri.res);
    if (!rule) return;
    if (!restored) {
        g_off_kept.fetch_add(1, std::memory_order_relaxed);
        if (DiagOnce("UNSTAMP-REFUSED", (int)rule))
            logger::write("UNSTAMP-REFUSED f=%u rule %08x res=%p: the hand-back has no CPU-only source for the game's "
                          "descriptor (view %llx) -- the switched-off mod's texture %p stays",
                g_frame_id, rule,
                (void*)ri.res, (unsigned long long)ri.view_handle, (void*)ri.swapped);
        return;
    }
    HeapKey src{};
    ResInfo sri{};
    if (!TrackedSlotOfCpu(ri.view_handle, &src) || !g_heap_to_res.find(src, sri) || !sri.swapped) return;
    g_off_kept.fetch_add(1, std::memory_order_relaxed);
    if (DiagOnce("UNSTAMP-NOOP", (int)rule))
        logger::write("UNSTAMP-NOOP f=%u rule %08x res=%p: the hand-back copied the game's descriptor from heap %llx "
                      "slot %u, itself stamped with %p -- the switched-off mod's texture stays",
            g_frame_id, rule,
            (void*)ri.res, (unsigned long long)src.heap, src.offset, (void*)sri.swapped);
}
