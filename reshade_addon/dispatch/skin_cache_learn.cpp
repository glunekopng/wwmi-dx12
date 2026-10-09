// skin_cache_learn.cpp -- the game's skin-cache dispatch for a mod character found by data flow, and the
// pool buffers it writes recorded (g_skincache_out). See dispatch_parts.h.
#include "../addon_common.h"
#include "../mods/claim_state.h"
#include "../mods/layout_registry.h"
#include "../hooks/resource_tracker.h"
#include "../rt/rt_hooks.h" // RT: the BLAS rings, the RTPAL recorder, RTBONES / RTSKINREAD
#include "dispatch_parts.h"

using namespace reshade::api;

void LearnSkinCache(const DispatchArgs& da, const CmdState& state)
{
    reshade::api::command_list* const cmd_list = da.cmd_list;
    const uint64_t                    cs_hash = da.cs_hash;
    const uint32_t                    group_count_x = da.group_count_x;
    const uint32_t                    group_count_y = da.group_count_y;
    const uint32_t                    group_count_z = da.group_count_z;
    if (!g_debug) return;
    {
        uint64_t pool_lo = g_skin_band_lo.load();
        uint64_t pool_hi = g_skin_band_hi.load();
        bool     consider;
        {
            std::lock_guard l(g_char_mesh_mtx);
            std::lock_guard l2(g_skincache_mtx);
            consider = (!g_char_mesh_vb0.empty() || RtSkinOutCount() != 0) &&
                (g_char_skin_cs.empty() || g_char_skin_cs.count(cs_hash));
        }
        if (consider) {
            struct Cand {
                uint64_t va;
                uint32_t size;
                bool     root;
            };
            constexpr uint32_t       kRootCount = 64;
            constexpr uint32_t       kTableCount = 16;
            constexpr uint32_t       kSlotCount = 64;
            constexpr uint32_t       kCandCap = kRootCount + kTableCount * kSlotCount;
            static thread_local Cand cand[kCandCap]; // 17 KB: per thread, not on the stack (C6262)
            uint32_t                 nc = 0;
            for (uint32_t p = 0; p < kRootCount; ++p)
                if (state.root[p].va) cand[nc++] = { state.root[p].va, 0u, true };
            {
                auto* dev = cmd_list->get_device();
                for (uint32_t p = 0; p < kTableCount; ++p) {
                    const auto       tbl = state.root[p].compute_table();
                    const TableStart ts = TableStartOf(dev, tbl);
                    if (!ts) continue;
                    for (uint32_t i = 0; i < kSlotCount; ++i) {
                        ResInfo ri;
                        if (!g_heap_to_res.find({ ts.heap, ts.base + i }, ri)) continue; // lock-free
                        if (ri.cached_va) cand[nc++] = { ri.cached_va, ri.size, false };
                    }
                }
            }
            bool reads_mesh = false;
            bool writes_rt_out = false;
            {
                std::lock_guard l(g_char_mesh_mtx); // EXACT bind-pose VB0 -> no heap noise
                for (uint32_t k = 0; k < nc && !reads_mesh; ++k) reads_mesh = g_char_mesh_vb0.count(cand[k].va) != 0;
            }
            const bool rt_on = RtSkinOutCount() != 0; // no ring recorded: RtIsSkinOut is false for all
            bool       rt_out[kCandCap] = {};
            if (rt_on)
                for (uint32_t k = 0; k < nc; ++k)
                    if ((rt_out[k] = RtIsSkinOut(cand[k].va, cand[k].root ? 1u : cand[k].size))) writes_rt_out = true;
            if (reads_mesh || writes_rt_out) {
                struct Buf {
                    uint64_t va;
                    uint32_t size;
                };
                std::vector<Buf> out_bufs;
                {
                    std::lock_guard lm(g_mem_mutex);
                    for (uint32_t k = 0; k < nc; ++k) {
                        const uint64_t va = cand[k].va;
                        const bool     pool = va >= pool_lo && va < pool_hi;
                        if (cand[k].root) {
                            if (rt_out[k]) out_bufs.push_back({ va, 0u });
                            if (pool) {
                                auto it = g_va_to_res.find(va);
                                out_bufs.push_back({ va, it != g_va_to_res.end() ? it->second.size : 0u });
                            }
                        } else if (rt_out[k] || pool)
                            out_bufs.push_back({ va, cand[k].size });
                    }
                }
                std::lock_guard l(g_skincache_mtx);
                int             added = 0;
                for (auto& b : out_bufs) {
                    bool dup = false;
                    for (auto& o : g_skincache_out)
                        if (o.va == b.va) {
                            dup = true;
                            break;
                        }
                    if (!dup) {
                        g_skincache_out.push_back({ b.va, b.size });
                        ++added;
                    }
                }
                if (g_char_skin_cs.insert(cs_hash).second)
                    logger::write("SKINCACHE: cs=%016llx skins our char (%s); poolOutputs=%zu newOutputs=%d totalOut=%zu groups=%u,%u,%u",
                        cs_hash, writes_rt_out ? "WRITES the vertex buffer a character's BLAS is built from -- RT" : "binds char mesh VB0",
                        out_bufs.size(), added, g_skincache_out.size(), group_count_x, group_count_y, group_count_z);
            }
        }
    }
}
