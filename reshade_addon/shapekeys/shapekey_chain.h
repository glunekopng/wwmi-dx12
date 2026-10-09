// The shapekey chain's seam to the rest of the addon: what the hooks call into, and the counters the
// overlay reads. The chain's own units and their couplings are in sk_internal.h.
#pragma once

#include <reshade.hpp>
#include <d3d12.h>
#include <wrl.h>
#include <mutex>
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cstdio>
#include <cassert>

#include "../../src/log.h"
#include "../../src/mod_loader.h"
#include "../../src/fnv.h"
#include "../addon_state.h"
#include "../mods/buffer_relocation.h" // g_buffer_reloc -- the chain binds relocated buffers
#include "../hooks/cmd_bind_hooks.h"   // ResolveBound -- the chain asks what is bound at a register
#include "../hooks/resource_tracker.h" // g_mem_mutex / g_res_to_va -- liveness before any dereference
#include "../mods/claim_state.h"       // g_geomods / ModGeomEnabled
#include "../mods/port_section.h"      // LogBudget -- the chain's per-namespace log allowance
#include "../config.h"
#include "../instrumentation/profiler.h"
#include "../instrumentation/diagnostics.h"
#include "../mods/ib_hash.h"
#include "../migoto/frame_analysis_driver.h"
#include "DrawCallInfo.h"
#include "../textures/texture_module.h"
#include "../migoto/port_api.h"
#include "../migoto/dx12_pipeline.h" // PipeState, SkCapSetCaller
#include "../migoto/dx12_bridge.h"   // GetDx12Bridge
#include "../instrumentation/morph_dump.h"
#include "../migoto/dx12_wrap.h" // AsShaderCode / ShaderCode
#include "../mods/skeleton.h"    // g_init_res_* -- the creation-event counters sk_resolve reports

using namespace reshade::api;

struct SkBinding {
    ID3D12Resource* res = nullptr;
    unsigned        bytes = 0;
};
struct SkBufferBinding : SkBinding {
    uint64_t va = 0;
};

// One register of a mod instance's published state; false = nothing live recorded (or reg out of range).
bool SkGetBinding(const std::wstring& ns, ID3D12Resource* inst, TargetClass kind, unsigned reg, SkBufferBinding* out);

uint8_t SkZeroBinding(const std::wstring& ns, ID3D12Resource* inst, TargetClass kind, unsigned reg,
    command_list* cmd_list, const char* why);
/* The three rates for one register, for the heartbeat (UAVCLEAR-RATE). */
void SkZeroStats(TargetClass kind, unsigned reg, unsigned long long* cleared,
    unsigned long long* not_ours, unsigned long long* not_ready);

bool SkSetSnap(const std::wstring& ns, ID3D12Resource* inst, unsigned reg, unsigned long long cs, unsigned tgy,
    ID3D12Resource* res, uint64_t va, unsigned bytes);
bool SkGetSnap(const std::wstring& ns, ID3D12Resource* inst, unsigned reg, unsigned long long cs, unsigned tgy,
    SkBufferBinding* out);

struct SkU0Ident {
    bool            shapekey = false; // this dispatch is one of the two overridden shapekey computes
    uint64_t        layout = 0;       // the compute root signature in force
    int             have = 0;         // a u0 table entry was found for that layout
    unsigned        param = 255;      // its root param ...
    unsigned        off = 0;          // ... and the descriptor offset within that table
    unsigned        size = 0;         // the resolved buffer's extent, 0 if it could not be established
    unsigned        desc = 0;         // crc32c of the reconstructed D3D11_BUFFER_DESC -- the ini's `hash =`
    int             declared = 0;     // exactly one on-screen mod claimed it (never a tie -- see the gate)
    int             owner = -1;       // that mod's index, or -1
    int             claims = 0;       // how many claimed it; >1 is a REFUSAL, not a tie to break
    int             live = 0;         // the resource is in g_res_to_va, so safe to dereference
    int             push = -1;        // PushGameComputeUav result, -1 = never attempted
    int             why = 1;          // 0 ok, 1 no u0 bound, 2 heap miss, 3 no resource, 4/5 extent failed
    uint64_t        heap = 0;
    unsigned        slot = 0;
    int             ri_age = -1;
    int             ri_src = 0;
    int             ri_buf = -1; // ResInfo::is_buffer  -- 0 here means cs-u0 resolved to a NON-BUFFER
    int             ri_tex = -1; // ResInfo::tex_cand
    int             ct_match = -1;
    int             ct_age = -1;
    int             ct_seqd = -1;
    int             by_owner = 0; // u0 came from the owner's declared hash, the slot having had no answer
    double          filter = 0.0; // the `cs` operand their engine evaluated for this dispatch
    unsigned        geo_lo = 0;
    unsigned        geo_hi = 0;
    int             geo = 0;
    ID3D12Resource* inst = nullptr; // the game's u0 this dispatch published under, before relocation
};

void SkResolveU0(command_list* cmd_list, uint64_t cs_hash,
    uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z,
    SkU0Ident& ck);

void SkRefreshModHashes();

// The dispatch scope, called by the generic compute hook around each dispatch (sk_queue.cpp).
void SkNoteDispatchList(const void* native_cl, uint32_t frame);
void SkBeginDispatchScope(uint64_t cs_hash, uint32_t group_count_y, const SkU0Ident& ck);
void SkNoteDrawVbs(int mod, ID3D12Resource* const* res, unsigned n);
void SkNoteChainWroteU0(ID3D12Resource* res, uint32_t frame);
bool SkChainWroteU0Recently(ID3D12Resource* res, uint32_t frame);
bool SkU0ResourceNow(command_list* cmd_list, ID3D12Resource** res, unsigned* size);

unsigned SkTakeQueueOutcome(uint8_t* reason);

void RunShapeKeyPasses(const std::wstring& ns, int mod_id, ID3D12Resource* inst, command_list* cmd_list);

// Resource lifetime: a destroyed resource must not survive in the chain's snapshots.
void SkInvalidateSnapshotFor(ID3D12Resource* dead);

inline std::atomic<unsigned> g_claim_ok{ 0 };        // exactly one on-screen mod declared this buffer
inline std::atomic<unsigned> g_claim_nodecl{ 0 };    // no shapekey CALLBACK section declares this hash
inline std::atomic<unsigned> g_claim_offscreen{ 0 }; // declared, but no claimant passed the 4-frame filter
inline std::atomic<unsigned> g_claim_multi{ 0 };     // two on-screen mods declare it -- refused, correctly
inline std::atomic<unsigned> g_u0tab_compute{ 0 };   // this dispatch's own compute table answered
inline std::atomic<unsigned> g_u0tab_fallback{ 0 };  // no compute table at that param -- used the union
inline std::atomic<unsigned> g_u0tab_none{ 0 };      // neither had anything

struct SkPendingList {
    std::wstring       ns;
    const void*        list;
    unsigned long long cs;
    unsigned           u0hash; // desc hash of the u0 resolved AT THE DISPATCH that queued this list
    unsigned           tgy;    // the game's thread_group_count_y there -- the batch selector
    int                owner_mod;
    ID3D12Resource*    inst; // SkU0Ident::inst of that dispatch; nullptr = it published nothing
};
extern std::vector<SkPendingList> g_sk_pending;
extern std::mutex                 g_sk_pending_mtx;

void on_execute_command_list(reshade::api::command_queue*, reshade::api::command_list* cl);
