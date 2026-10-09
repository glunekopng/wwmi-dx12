#pragma once
#include <reshade.hpp> // the pipeline_layout_param the walkers below take
#include <d3d12.h>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include "target_class.h"            // RegisterFileWidth / TargetClass -- LayoutRegMap's array widths
#include "../instrumentation/diag.h" // g_frame_id -- TableStartOf's memo

struct LayoutTableParam {
    uint8_t  param;
    uint8_t  kind;
    uint8_t  is_srv;
    uint32_t count;
    uint32_t base_reg;
};
inline std::unordered_map<uint64_t, std::vector<LayoutTableParam>> g_layout_tables;
inline std::mutex                                                  g_layout_tables_mtx;

constexpr uint32_t kTexScanMin = 128;
constexpr uint32_t kTexScanMax = 512;
inline void        LayoutTexScanLens(uint64_t layout, uint32_t (&out)[64])
{
    for (uint32_t& n : out) n = kTexScanMin;
    if (!layout) return;
    std::lock_guard l(g_layout_tables_mtx);
    auto            it = g_layout_tables.find(layout);
    if (it == g_layout_tables.end()) return;
    for (const auto& t : it->second) {
        if (t.param >= 64 || !t.is_srv) continue;
        if (t.count == 0)
            out[t.param] = kTexScanMax;
        else if (t.count > kTexScanMin)
            out[t.param] = t.count < kTexScanMax ? t.count : kTexScanMax;
    }
}

struct TableStart {
    uint64_t              heap = 0;
    uint32_t              base = 0;
    explicit              operator bool() const { return heap != 0; }
    ID3D12DescriptorHeap* Heap() const { return reinterpret_cast<ID3D12DescriptorHeap*>(heap); }
};
inline TableStart TableStartOf(reshade::api::device* dev, reshade::api::descriptor_table tab)
{
    TableStart ts;
    if (!tab.handle) return ts;
    struct Memo {
        uint64_t   table = 0;
        uint32_t   frame = 0;
        TableStart ts;
    };
    thread_local Memo     t_memo[16];
    thread_local unsigned t_next = 0;
    for (const Memo& m : t_memo)
        if (m.table == tab.handle && m.frame == g_frame_id) return m.ts;
    reshade::api::descriptor_heap dh = { 0 };
    dev->get_descriptor_heap_offset(tab, 0, 0, &dh, &ts.base);
    ts.heap = dh.handle;
    Memo& m = t_memo[t_next++ & 15];
    m.table = tab.handle;
    m.frame = g_frame_id;
    m.ts = ts;
    return ts;
}

inline std::mutex                        g_layout_mutex;
inline std::unordered_map<uint64_t, int> g_layout_u0cs_root; // compute UAV u0 as a ROOT param, -1 = none

struct CsCbvRoots {
    int reg[16];
};

struct U0CsTable {
    uint8_t  param;
    uint32_t offset;
}; // ...or as (table param, descriptor offset)
inline std::unordered_map<uint64_t, U0CsTable> g_layout_u0cs_table;

struct CsUavTables {
    U0CsTable t[8];
    bool      has[8];
};

struct RegBind {
    uint8_t  param = 0xFF;     // root parameter index
    uint32_t offset = 0;       // descriptor offset WITHIN the table (0 for a root/push descriptor)
    bool     is_table = false; // false = root descriptor (read from root[].va / CmdCbv), true = table
};
struct LayoutRegMap {
    RegBind t_cs[RegisterFileWidth(TargetClass::ShaderResource)];
    RegBind u_cs[RegisterFileWidth(TargetClass::UnorderedAccessView)];
    RegBind b_cs[RegisterFileWidth(TargetClass::ConstantBuffer)]; // compute-visible
    RegBind t_gfx[RegisterFileWidth(TargetClass::ShaderResource)];
    RegBind u_gfx[RegisterFileWidth(TargetClass::UnorderedAccessView)];
    RegBind b_gfx[RegisterFileWidth(TargetClass::ConstantBuffer)]; // graphics-visible (vs/ps/...)
    RegBind t_vs[RegisterFileWidth(TargetClass::ShaderResource)];
    RegBind t_ps[RegisterFileWidth(TargetClass::ShaderResource)]; // per stage: the frame dump's vs-tN / ps-tN
    RegBind b_vs[RegisterFileWidth(TargetClass::ConstantBuffer)]; // per stage: the game declares one root
    bool    built = false;
};
inline std::unordered_map<uint64_t, LayoutRegMap> g_layout_regmap;
inline std::mutex                                 g_layout_regmap_mtx;

extern std::unordered_map<uint64_t, int>        g_layout_b4_root;      // CBV b4 -- the CURRENT skeleton
extern std::unordered_map<uint64_t, int>        g_layout_b3_root;      // ...and the previous one
extern std::unordered_map<uint64_t, int>        g_layout_b2_root;      // CBV b2 -- the drawn object's own: one address per copy
extern std::unordered_map<uint64_t, int>        g_layout_b0cs_root;    // ...the compute-visible one
extern std::unordered_map<uint64_t, CsCbvRoots> g_layout_cs_cbv_roots; // b0..b15, compute

// One entry of a per-layout root map, or -1 when the layout has none.
inline int LayoutRoot(const std::unordered_map<uint64_t, int>& m, uint64_t layout)
{
    std::lock_guard l(g_layout_mutex);
    auto            it = m.find(layout);
    return it == m.end() ? -1 : it->second;
}
// The layout's u0 table entry; false when it has none (*out untouched).
inline bool LayoutU0Table(uint64_t layout, U0CsTable* out)
{
    std::lock_guard l(g_layout_mutex);
    auto            it = g_layout_u0cs_table.find(layout);
    if (it == g_layout_u0cs_table.end()) return false;
    *out = it->second;
    return true;
}

uint32_t LayoutParamRangeCount(const reshade::api::pipeline_layout_param& p);
bool     LayoutParamRange(const reshade::api::pipeline_layout_param& p, uint32_t i,
    reshade::api::descriptor_range& out, bool* is_table = nullptr,
    reshade::api::descriptor_range_flags* flags = nullptr);
bool     LayoutParamIsRootDescriptor(const reshade::api::pipeline_layout_param& p);

// Which (root param, table offset) feeds compute UAV register `reg`, walking the declared ranges.
bool FindCsUavTable(uint32_t count, const reshade::api::pipeline_layout_param* params, uint32_t reg,
    U0CsTable* out);

void on_init_pipeline_layout(reshade::api::device*, uint32_t count,
    const reshade::api::pipeline_layout_param* params,
    reshade::api::pipeline_layout              layout);
void on_destroy_pipeline_layout(reshade::api::device*, reshade::api::pipeline_layout layout);
