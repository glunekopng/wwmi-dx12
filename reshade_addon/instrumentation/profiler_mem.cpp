// profiler_mem.cpp -- the profiler's once-a-second memory sample (GPU pools, CPU heap, our tables).
// See present_parts.h.
#include "../addon_common.h"
#include "../mods/claim_state.h"
#include "../hooks/resource_tracker.h"
#include "../present/present_parts.h"
#include <psapi.h>

using namespace reshade::api;

void SampleMemoryOncePerSecond()
{
    static int64_t last = 0;
    static int64_t first = 0;
    static bool    was_enabled = false;
    if (g_prof_enabled != was_enabled) {
        was_enabled = g_prof_enabled;
        last = 0;
        first = 0;
        g_prof_mem_have = false; // next sample becomes the new baseline (ProfMemFold latches it)
    }
    if (!g_prof_enabled) return;

    const int64_t freq = prof_qpc_freq();
    const int64_t now = prof_qpc_now();
    if (last && (now - last) < freq) return; // ~1s cadence
    if (!first) first = now;
    last = now;

    ProfMem s{};

    // ---- GPU pools -------------------------------------------------------------------------------
    static IDXGIAdapter3* s_adapter = nullptr;
    static ID3D12Device*  s_adapter_for = nullptr; // which device the cached adapter belongs to
    if (g_d3d12_device && s_adapter_for != g_d3d12_device) {
        if (s_adapter) {
            s_adapter->Release();
            s_adapter = nullptr;
        }
        s_adapter_for = g_d3d12_device;
        IDXGIFactory4* fac = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&fac))) && fac) {
            IDXGIAdapter1* a1 = nullptr;
            if (SUCCEEDED(fac->EnumAdapterByLuid(g_d3d12_device->GetAdapterLuid(), IID_PPV_ARGS(&a1))) && a1) {
                a1->QueryInterface(IID_PPV_ARGS(&s_adapter)); // IDXGIAdapter3 = the one with the budget API
                a1->Release();
            }
            fac->Release();
        }
    }
    if (s_adapter) {
        DXGI_QUERY_VIDEO_MEMORY_INFO vi{};
        if (SUCCEEDED(s_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vi))) {
            s.vram_budget = vi.Budget;
            s.vram_usage = vi.CurrentUsage;
        }
        if (SUCCEEDED(s_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &vi))) {
            s.host_budget = vi.Budget;
            s.host_usage = vi.CurrentUsage;
        }
    }

    // ---- CPU heap --------------------------------------------------------------------------------
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        s.proc_private = pmc.PrivateUsage;
        s.proc_ws = pmc.WorkingSetSize;
    }

    {
        std::lock_guard lock(g_mem_mutex);
        s.n_va_to_res = (uint32_t)g_va_to_res.size();
        s.n_res_to_va = (uint32_t)g_res_to_va.size();
    }
    {
        CmdLock lc;
        s.n_cmd_state = (uint32_t)g_cmd_state.size();
    }
    {
        std::lock_guard l(g_sk_pending_mtx);
        s.n_sk_pending = (uint32_t)g_sk_pending.size();
    }
    s.n_geomods = (uint32_t)g_geomods.size();
    {
        uint32_t heaps = 0;
        uint64_t slots = 0;
        g_heap_to_res.for_each_heap([&](uint64_t, uint64_t, uint32_t n, bool, FlatHeapMap::Slot*) { ++heaps; slots += n; return false; });
        s.n_heap_maps = heaps;
        s.heap_map_bytes = slots * sizeof(FlatHeapMap::Slot);
        s.n_heap_rejected = g_heap_to_res.rejected.load(std::memory_order_relaxed);
        s.n_heap_destroyed = g_heap_to_res.destroyed.load(std::memory_order_relaxed);
    }

    MigotoPort::PortHeapStats(&s.staging_used, &s.staging_cap, &s.staging_freed,
        &s.n_rootsigs, &s.n_psos);

    ProfMemFold(s, (double)(now - first) / (double)freq);
}
