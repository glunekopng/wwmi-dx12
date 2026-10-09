// ===========================================================================
//  diagnostics.cpp -- debug-only paths (gated by g_debug in diag.h). See
//  diagnostics.h. Shader bytecode cache, the clone/LOD per-draw logging, and
//  the hand-off of each draw to the ported engine's frame analysis.
// ===========================================================================
#include "diagnostics.h"
#include "draw_dump_publish.h"
#include "../addon_state.h"
#include "../mods/skeleton.h"
#include "../mods/claim_state.h" // InSkinBand
#include "../textures/texture_module.h"
#include "../config.h"
#include "../../src/log.h"
#include <cstdio>
#include "../migoto/frame_analysis_driver.h"
#include "../migoto/dx12_bridge.h" // DumpDir
#include "DrawCallInfo.h"

#include <cstring>
#include <utility>
#include <ctime>
#include <string>

using namespace reshade::api;

void CacheShaderBytecode(uint64_t hash, const void* code, size_t code_size, const char* stage)
{
    if (!code || code_size == 0 || code_size > 10u * 1024u * 1024u) return;
    std::lock_guard lock(g_shader_cache_mutex);
    if (g_shader_cache.count(hash)) return;    // already cached
    if (g_shader_cache.size() >= 4096) return; // cap (every pipeline's shaders are cached)
    CachedShader cs;
    cs.stage[0] = stage[0];
    cs.stage[1] = stage[1];
    cs.stage[2] = 0;
    cs.stage[3] = 0;
    cs.code.resize(code_size);
    memcpy(cs.code.data(), code, code_size);
    g_shader_cache[hash] = std::move(cs);
}

static std::wstring DumpSubdir()
{
    wchar_t ts[64];
    time_t  t = time(nullptr);
    tm      lt;
    localtime_s(&lt, &t);
    wcsftime(ts, 64, L"dump_%Y%m%d_%H%M%S", &lt);
    std::wstring d = MigotoPort::DumpDir() + L"\\" + ts;
    CreateDirectoryW(MigotoPort::DumpDir().c_str(), nullptr);
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

void DumpCachedShaders()
{
    std::wstring sub = DumpSubdir() + L"\\shaders";
    CreateDirectoryW(sub.c_str(), nullptr);
    std::vector<std::pair<uint64_t, CachedShader>> snapshot;
    {
        std::lock_guard lock(g_shader_cache_mutex);
        for (auto& kv : g_shader_cache) snapshot.emplace_back(kv.first, kv.second);
    }
    for (auto& kv : snapshot) {
        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%s\\shader_%S_%016llx.bin", sub.c_str(), kv.second.stage, kv.first);
        FILE* f = nullptr;
        _wfopen_s(&f, path, L"wb");
        if (f) {
            fwrite(kv.second.code.data(), 1, kv.second.code.size(), f);
            fclose(f);
        }
        logger::write("Shader dump: %s %016llx -> %u bytes -> %S",
            kv.second.stage, kv.first, (uint32_t)kv.second.code.size(), path);
    }
}

struct ShaderSeen {
    char     stage;
    uint32_t draws;
    uint32_t max_indices;
    uint64_t partner;
};
static std::mutex                               g_shaderdiff_mtx;
static std::unordered_map<uint64_t, ShaderSeen> g_shaderdiff_now;
static std::unordered_set<uint64_t>             g_shaderdiff_base;
static bool                                     g_shaderdiff_have_base = false;
static void                                     ShaderDiffAdd(uint64_t h, char stage, uint64_t partner, uint32_t index_count)
{
    if (!h) return;
    auto it = g_shaderdiff_now.try_emplace(h, ShaderSeen{ stage, 0, 0, partner }).first;
    ++it->second.draws;
    if (index_count > it->second.max_indices) it->second.max_indices = index_count;
}
void ShaderDiffNote(command_list* cmd_list, uint64_t vs_hash, uint32_t index_count)
{
    if (g_shaderdiff_arm.load(std::memory_order_relaxed) <= 0) return;
    uint64_t ps_hash = 0;
    cmd_list->get_private_data(s_iid_ps_hash, &ps_hash);
    std::lock_guard l(g_shaderdiff_mtx);
    ShaderDiffAdd(vs_hash, 'v', ps_hash, index_count);
    ShaderDiffAdd(ps_hash, 'p', vs_hash, index_count);
}
static void ShaderDiffFinish()
{
    std::lock_guard l(g_shaderdiff_mtx);
    if (!g_shaderdiff_have_base) {
        g_shaderdiff_base.clear();
        for (const auto& kv : g_shaderdiff_now) g_shaderdiff_base.insert(kv.first);
        g_shaderdiff_have_base = true;
        logger::write("SHADERDIFF baseline: %zu shaders drew in 60 frames -- press D14 again in the other state",
            g_shaderdiff_base.size());
    } else {
        unsigned fresh = 0;
        for (const auto& [h, s] : g_shaderdiff_now) {
            if (g_shaderdiff_base.count(h)) continue;
            ++fresh;
            logger::write("SHADERDIFF new %cs=%016llx draws=%u max_indices=%u with %cs=%016llx", s.stage,
                (unsigned long long)h, s.draws, s.max_indices, s.stage == 'v' ? 'p' : 'v', (unsigned long long)s.partner);
        }
        logger::write("SHADERDIFF: %u of %zu shaders drew only in the second state", fresh, g_shaderdiff_now.size());
        g_shaderdiff_have_base = false;
    }
    g_shaderdiff_now.clear();
}

void DiagOnDrawIndexed(command_list* cmd_list, ID3D12GraphicsCommandList*,
    uint64_t vs_hash, uint32_t first_index, uint32_t index_count, MergedComp* mc,
    int32_t  vertex_offset, int, int,
    uint32_t instance_count, uint32_t first_instance)
{
    bool fd_claimed = (mc != nullptr);
    bool fd_charband = false;
    if (MigotoPort::FrameAnalysisArmed() && !fd_claimed) {
        uint64_t vb0 = 0;
        {
            vb0 = CmdGfx(cmd_list).vb_va[0];
        }
        fd_charband = InSkinBand(vb0);
    }
    if (MigotoPort::FrameAnalysisWantGameDraw(fd_claimed, fd_charband) && index_count >= 3) {
        DrawCallInfo fci{};
        fci.type = (instance_count > 1) ? DrawCall::DrawIndexedInstanced : DrawCall::DrawIndexed;
        fci.IndexCount = index_count;
        fci.InstanceCount = instance_count;
        fci.FirstIndex = first_index;
        fci.FirstVertex = (UINT)vertex_offset;
        fci.FirstInstance = first_instance;
        uint64_t fa_ps = 0;
        cmd_list->get_private_data(s_iid_ps_hash, &fa_ps);
        // The GAME's own geometry and targets, not the last injected draw's leftovers.
        PublishGameDrawForDump(cmd_list, /*injected*/ false, nullptr, 0, 0, 0);
        MigotoPort::FrameAnalysisAfterDraw(false, &fci, vs_hash, fa_ps, 0);
        PublishGameDrawForDumpEnd();
    }

    if ((g_log_merged.load() > 0 || g_log_lod.load()) && index_count >= 64) {
        uint64_t vb0 = 0;
        {
            vb0 = CmdGfx(cmd_list).vb_va[0];
        }
        bool char_pool = (vb0 >= 0x300000000ULL && vb0 < 0x400000000ULL);
        bool at_comp = (g_first_to_comp.find(FirstCountKey(first_index, index_count)) != g_first_to_comp.end());
        if (char_pool || at_comp || index_count >= 3000) {
            uint64_t ps_hash = 0;
            cmd_list->get_private_data(s_iid_ps_hash, &ps_hash);
            uint32_t   bone_roots = BoneRootMask(cmd_list);
            uint64_t   key = vs_hash ^ ((uint64_t)first_index << 1) ^ ((uint64_t)index_count << 20);
            const bool fresh = DiagOnce("CLONEDIAG", (int)(key ^ (key >> 32)));
            uint64_t   bone_va = 0;
            {
                auto it = &CmdCbv(cmd_list);
                if (it != nullptr)
                    for (int rp = 0; rp < 16; ++rp)
                        if (bone_roots & (1u << rp)) {
                            bone_va = it->binds[rp].va;
                            break;
                        }
            }
            if (fresh) logger::write("CDRAW vs=%016llx ps=%016llx first=%u count=%u vb0=%llx pool=%d atcomp=%d claimed=%d comp=%d boneRoots=%04x boneVA=%llx",
                vs_hash, ps_hash, first_index, index_count, vb0, (int)char_pool, (int)at_comp,
                (int)(mc != nullptr), mc ? mc->comp_idx : -1, bone_roots, bone_va);
        }
    }

    // --- Diagnostic: log all substantial draws (find LOD hashes at distance) ---
    if (g_log_all_draws.load() > 0 && index_count >= 500) {
        // Tag character-pool draws: VB[0] VA inside the known skin-cache pool range
        uint64_t vb0 = 0;
        {
            vb0 = CmdGfx(cmd_list).vb_va[0];
        }
        bool charPool = (vb0 >= 0x3f0000000ULL && vb0 < 0x400000000ULL); // CS output pool band
        logger::write("DRAW vs=%016llx first=%u count=%u vb0=%llx%s",
            vs_hash, first_index, index_count, vb0, charPool ? "  <<CHAR-POOL" : "");
    }

    if (vs_hash == 0xbd50930e536be3eaULL || vs_hash == 0x1835840f4e3ff12aULL || vs_hash == 0xec783ffd78365180ULL) {
        auto const&     state = CmdGfx(cmd_list);
        std::lock_guard lock_vb(g_char_vb_mutex);
        if (state.vb_va[0] != 0) g_char_vb_vas.insert(state.vb_va[0]);
    }

    if (vs_hash == 0x1758b5d5982ddd63ULL || vs_hash == 0xbb0ac53ff09064e6ULL) {
        const uint64_t lk = vs_hash ^ ((uint64_t)first_index << 1) ^ ((uint64_t)index_count << 20);
        if (DiagOnce("LODDIAG", (int)(lk ^ (lk >> 32)))) {
            auto const& st = CmdGfx(cmd_list);
            logger::write("LODDIAG vs=%016llx first=%u count=%u ib_va=%llx ib_bytes=%u",
                vs_hash, first_index, index_count, st.ib_va, st.ib_index_bytes);
            for (int s = 0; s < 8; ++s)
                if (st.vb_va[s]) logger::write("  LODVB[%d] va=%llx stride=%u size=%u", s, st.vb_va[s], st.vb_stride[s], st.vb_size[s]);
            for (int rp = 0; rp < 64; ++rp)
                if (st.tables[rp].handle) logger::write("  LODTABLE root[%d] handle=%llx", rp, st.tables[rp].handle);
            auto cit = &CmdCbv(cmd_list);
            if (cit != nullptr) {
                for (int rp = 0; rp < 64; ++rp) {
                    auto& b = cit->binds[rp];
                    if (!b.res || !b.va) continue;
                    // Existing mappings only, as before: a diagnostic does not map new resources.
                    float      m[12];
                    const bool bonelike = SnapshotGameCB(b.res, b.va, (uint8_t*)m, sizeof(m), false) == sizeof(m) && LooksLikeBone(m);
                    logger::write("  LODCBV root[%d] va=%llx size=%u bonelike=%d", rp, b.va, b.size, (int)bonelike);
                }
            }
        }
    }
}

void DiagOnPresent(ID3D12CommandQueue*)
{
    if (g_dump_shaders_requested.exchange(false)) DumpCachedShaders();
    if (g_shaderdiff_arm.load() > 0 && g_shaderdiff_arm.fetch_sub(1) == 1) ShaderDiffFinish();
    {
        int s = g_log_all_draws.load();
        if (s > 0) g_log_all_draws.store(s - 1);
    }
    {
        int s = g_texflip_arm.load();
        if (s > 0) g_texflip_arm.store(s - 1);
    }
    {
        int s = g_texnoswap_arm.load();
        if (s > 0) g_texnoswap_arm.store(s - 1);
    }
    {
        int s = g_log_merged.load();
        if (s > 0) g_log_merged.store(s - 1);
    }
    {
        std::lock_guard lock(g_char_vb_mutex);
        g_char_vb_vas.clear();
    }
}
