// skeleton_dump.cpp -- the H4 / H5 skeleton readbacks (see skeleton.h).
#include "../addon_common.h"
#include "../mods/skeleton.h"
#include "../../src/d3d12_buffer.h"
#include "../../src/d3d12_barrier.h"

// What the last component draw published as vs-cb4 / vs-cb3, kept for the chain dump.
static ID3D12Resource* g_dump_cb4_res = nullptr;
static uint64_t        g_dump_cb4_va = 0;
static ID3D12Resource* g_dump_cb3_res = nullptr;
static uint64_t        g_dump_cb3_va = 0;
static int             g_dump_cb4_root = -1;
static int             g_dump_cb3_root = -1;
void                   SkelDumpNoteDrawCbs(ID3D12Resource* cb4_res, uint64_t cb4_va, int cb4_root,
    ID3D12Resource* cb3_res, uint64_t cb3_va, int cb3_root)
{
    g_dump_cb4_res = cb4_res;
    g_dump_cb4_va = cb4_va;
    g_dump_cb3_res = cb3_res;
    g_dump_cb3_va = cb3_va;
    g_dump_cb4_root = cb4_root;
    g_dump_cb3_root = cb3_root;
}
struct ChainSrc {
    std::wstring    label;
    ID3D12Resource* res;
    UINT64          off;
};
void SkeletonChainDumpTick(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, ID3D12CommandQueue* q)
{
    static std::vector<std::pair<std::wstring, ID3D12Resource*>> s_rbs; // label -> readback
    static uint32_t                                              s_ready_f = 0;
    static const UINT64                                          kBytes = 64 * 16; // 64 float4 per source = ~21 bones

    // ---- phase 2 ----
    if (!s_rbs.empty() && g_frame_id >= s_ready_f) {
        logger::write("=== SKELETON CHAIN DUMP (frame %u) ===", g_frame_id);
        for (auto& e : s_rbs) {
            void*       map = nullptr;
            D3D12_RANGE rr{ 0, (SIZE_T)kBytes };
            if (SUCCEEDED(e.second->Map(0, &rr, &map)) && map) {
                const float* f = (const float*)map;
                int          nz = 0;
                for (int k = 0; k < 64 * 4; ++k)
                    if (f[k] != 0.0f) ++nz;
                logger::write("  %-46ls nonzero=%3d/256", e.first.c_str(), nz);
                for (int row = 0; row < 3; ++row)
                    logger::write("      [%8.3f %8.3f %8.3f %8.3f]",
                        f[row * 4 + 0], f[row * 4 + 1], f[row * 4 + 2], f[row * 4 + 3]);
                D3D12_RANGE none{ 0, 0 };
                e.second->Unmap(0, &none);
            }
            e.second->Release();
        }
        s_rbs.clear();

        // ---- the variables that steer all of it ----
        logger::write("  --- engine variables (skeleton chain) ---");
        PortApi::ForEachVariable([](const wchar_t* fq, float v, bool persist, void*) {
            if (!fq) return;
            std::wstring n(fq);
            for (auto& c : n) c = (wchar_t)towlower(c);
            static const wchar_t* kWanted[] = { L"state_id", L"vg_offset", L"vg_count", L"mod_enabled",
                L"object_detected", L"mod_id", L"custom_mesh_scale",
                L"merge_status", L"blend_remaps_initialized" };
            for (const wchar_t* w : kWanted)
                if (n.find(w) != std::wstring::npos) {
                    logger::write("    %-58ls = %g%s", fq, v, persist ? "  (persist)" : "");
                    return;
                }
        },
            nullptr);
        logger::write("=== END SKELETON CHAIN DUMP ===");
        return;
    }
    if (!s_rbs.empty()) return; // recorded, waiting
    if (!g_skelchain_req.exchange(false) || !dev || !cl || !q) return;

    // ---- phase 1: collect every source, then record one copy each ----
    std::vector<ChainSrc> srcs;
    if (g_dump_cb4_res) srcs.push_back({ L"INPUT  game vs-cb4 (merger cs-cb8)", g_dump_cb4_res, g_dump_cb4_va });
    if (g_dump_cb3_res) srcs.push_back({ L"INPUT  game vs-cb3", g_dump_cb3_res, g_dump_cb3_va });
    struct Acc {
        std::vector<ChainSrc>* v;
    } acc{ &srcs };
    PortApi::ForEachCustomResource([](const wchar_t* fq, const wchar_t*, int, bool, ID3D12Resource* dx12, void* u) {
        if (!fq || !dx12) return;
        std::wstring n(fq);
        for (auto& c : n) c = (wchar_t)towlower(c);
        if (n.find(L"skeleton") == std::wstring::npos && n.find(L"blendremap") == std::wstring::npos) return;
        ((Acc*)u)->v->push_back({ fq, dx12, 0 });
    },
        &acc);

    if (srcs.empty()) {
        logger::write("SKELCHAIN: nothing to dump (no bone CB seen, no skeleton resources)");
        return;
    }

    logger::write("SKELCHAIN: recording %zu source(s); cb4root=%d cb3root=%d; reading at frame %u",
        srcs.size(), g_dump_cb4_root, g_dump_cb3_root, g_frame_id + 4);
    for (ChainSrc& c : srcs) {
        bool is_game_cb = false;
        {
            const bool is_cb = (c.res == g_dump_cb4_res || c.res == g_dump_cb3_res);
            if (is_cb) {
                D3D12_HEAP_PROPERTIES chp{};
                D3D12_HEAP_FLAGS      chf{};
                if (SUCCEEDED(c.res->GetHeapProperties(&chp, &chf)) && chp.Type == D3D12_HEAP_TYPE_UPLOAD)
                    is_game_cb = true; // GENERIC_READ: legal copy source, no barrier
                else {
                    logger::write("  SKELCHAIN skip '%ls' -- game CB is not on an UPLOAD heap and has no "
                                  "tracked state; a guessed barrier can remove the device",
                        c.label.c_str());
                    continue;
                }
            }
        }
        D3D12_RESOURCE_STATES st{};
        if (!is_game_cb && !CopySourceState(MigotoPort::QueryResourceState(c.res), &st)) {
            logger::write("  SKELCHAIN skip '%ls' -- no tracked state (never guess a barrier)", c.label.c_str());
            continue;
        }
        const D3D12_RESOURCE_DESC rd = c.res->GetDesc();
        const UINT64              base = c.res->GetGPUVirtualAddress();
        UINT64                    off = 0;
        if (c.off && c.off >= base && c.off < base + rd.Width) off = c.off - base; // VA -> byte offset
        if (off + kBytes > rd.Width) continue;

        ID3D12Resource* rb = nullptr;
        if (FAILED(CreateD3D12Buffer(dev, D3D12_HEAP_TYPE_READBACK, kBytes, D3D12_RESOURCE_STATE_COPY_DEST, &rb)))
            continue;

        const auto copy = [&] { cl->CopyBufferRegion(rb, 0, c.res, off, kBytes); };
        if (is_game_cb)
            copy(); // UPLOAD heap, GENERIC_READ: a legal copy source as-is
        else
            CopyAsSource(cl, c.res, st, copy);
        s_rbs.emplace_back(c.label, rb);
    }
    s_ready_f = g_frame_id + 4;
}

void SkeletonDumpTick(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, ID3D12CommandQueue* q)
{
    static ID3D12Resource* s_rb = nullptr;
    static std::wstring    s_fq;
    static uint32_t        s_ready_f = 0;
    static const UINT64    kBytes = 64 * 16; // 64 float4 = ~21 bones

    // ---- phase 2: the copy has been submitted and retired; read it ----
    if (s_rb && g_frame_id >= s_ready_f) {
        void*       map = nullptr;
        D3D12_RANGE rr{ 0, (SIZE_T)kBytes };
        if (SUCCEEDED(s_rb->Map(0, &rr, &map)) && map) {
            const float* f = (const float*)map;
            int          nonzero = 0;
            for (int k = 0; k < 64 * 4; ++k)
                if (f[k] != 0.0f) ++nonzero;
            logger::write("SKELDUMP '%ls' -- first 6 bones (float3x4 rows):", s_fq.c_str());
            for (int bone = 0; bone < 6; ++bone) {
                const float* r = f + bone * 12;
                logger::write("  bone %2d: [%8.3f %8.3f %8.3f %8.3f] [%8.3f %8.3f %8.3f %8.3f] "
                              "[%8.3f %8.3f %8.3f %8.3f]",
                    bone,
                    r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11]);
            }
            logger::write("SKELDUMP: %d of %d floats non-zero%s", nonzero, 64 * 4,
                nonzero == 0 ? "  *** ALL ZERO ***" : "");
            D3D12_RANGE none{ 0, 0 };
            s_rb->Unmap(0, &none);
        }
        s_rb->Release();
        s_rb = nullptr;
        return;
    }
    if (s_rb) return; // recorded, still waiting
    if (!g_skeldump_req.exchange(false) || !dev || !cl || !q) return;

    // ---- phase 1: find it and record the copy ----
    struct Ctx {
        std::wstring    fq;
        ID3D12Resource* res;
    } ctx{ std::wstring(), nullptr };
    PortApi::ForEachCustomResource([](const wchar_t* fq, const wchar_t*, int, bool, ID3D12Resource* dx12, void* u) {
        Ctx* c = (Ctx*)u;
        if (c->res || !dx12 || !fq) return;
        std::wstring n(fq);
        for (auto& ch : n) ch = (wchar_t)towlower(ch);
        if (n.find(L"mergedskeletonrw") != std::wstring::npos && n.find(L"extra") == std::wstring::npos &&
            n.find(L"remap") == std::wstring::npos) {
            c->fq = fq;
            c->res = dx12;
        }
    },
        &ctx);
    if (!ctx.res) {
        logger::write("SKELDUMP: no substantiated ...MergedSkeletonRW found");
        return;
    }

    if (FAILED(CreateD3D12Buffer(dev, D3D12_HEAP_TYPE_READBACK, kBytes, D3D12_RESOURCE_STATE_COPY_DEST, &s_rb))) {
        logger::write("SKELDUMP: readback alloc failed");
        s_rb = nullptr;
        return;
    }

    D3D12_RESOURCE_STATES before{};
    if (!CopySourceState(MigotoPort::QueryResourceState(ctx.res), &before)) {
        logger::write("SKELDUMP: '%ls' has no tracked resource state -- skipping rather than guessing "
                      "(a wrong StateBefore removes the device)",
            ctx.fq.c_str());
        s_rb->Release();
        s_rb = nullptr;
        return;
    }
    CopyAsSource(cl, ctx.res, before, [&] { cl->CopyBufferRegion(s_rb, 0, ctx.res, 0, kBytes); });
    s_fq = ctx.fq;
    s_ready_f = g_frame_id + 4; // well past this frame's submit + any in-flight depth
    logger::write("SKELDUMP: copy recorded for '%ls' (tracked state 0x%x); reading at frame %u",
        s_fq.c_str(), (unsigned)before, s_ready_f);
}
