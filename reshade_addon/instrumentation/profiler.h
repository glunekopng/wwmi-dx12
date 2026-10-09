#pragma once
#include <cstdio>
#include <atomic>
#include <cstdint>
#include <windows.h>
#include "../migoto/prof_core.h" // ProfScope / PROF, the accumulators, ProfGpuWork, ProfLibStep
#include "diag.h"                // SetDiagnostics -- a dump runs with debug and logging off
#include "gpu_timer.h"           // g_gt_*: the GPU ms the timestamp brackets read back this frame
#include "hot_path.h"            // HotPath: the dump's HOT PATH section
#include "../migoto/port_api.h"  // PortApi::GetGpuWorkTotals -- the dump's GPU WORK section (plain types only)

enum ProfBucket {
    PROF_DescUpdate = 0, // on_update_descriptor_tables (GlobalTexSwap per SRV placement)
    PROF_DescCopy,       // on_copy_descriptor_tables   (TOP-LEVEL total)
    PROF_CopyBufToTex,   // on_copy_buffer_to_texture   (upload identity hash)
    PROF_CB_Alloc,       //   - the tight mip0 buffer: allocation + zero fill (nests wherever an upload is hashed)
    PROF_CB_Copy,        //   - its rows read out of the upload heap (write-combined)
    PROF_CB_Crc,         //   - the crc32c over it
    PROF_CB_Map,         //   - Map/Unmap of the upload buffer
    PROF_CB_TexLod,      //   - TexLodOnSample: the LOD signature + candidate scan
    PROF_CopyTexRegion,  // on_copy_texture_region      (mip-shift lineage propagation)
    PROF_Barrier,        // on_barrier                  (resource-state tracking)
    PROF_BindDescTables, // on_bind_descriptor_tables
    PROF_BindPipeline,   // on_bind_pipeline
    PROF_DrawIndexed,    // on_draw_indexed             (geometry claim / mesh inject) -- TOP-LEVEL total
    PROF_DI_TexScan,     //   - per-draw 128-slot descriptor/tex-hash scans (TEXOWN + claim-record + char_tex)
    PROF_DI_RfxDecide,   //   - RabbitFX SetTextures decide + bind (shadow-heap mirror)
    PROF_DI_ClaimInject, //   - claim decision + skeleton/bone-CB capture bookkeeping
    PROF_DI_BoneHash,    //     - HashBoneCB: skeleton-content hash (nests wherever called -- ClaimInject, LOD pin, probes).
    PROF_DI_Inject,      //   - post-claim: skin CB bind + PSO + replacement DrawIndexedInstanced replays
    PROF_DI_ShapeKeys,   //     - RunShapeKeyPasses TOTAL (once per mod per frame, inside Inject)
    PROF_SK_OwnBuf,      //       - the g_va_to_res declared-hash scan (stable per-mod cs-u0 bind)
    PROF_SK_Lists,       //       - RunResolvedCommandList x N (Setup/Load/Multiply/Apply)
    PROF_SK_Cb0,         //         - SkGetSnap + PushGameConstantBuffer (this pass's own cb0)
    PROF_SK_Run,         //         - PortApi::RunResolvedCommandList: THEIR interpreter under EngineLock; IssueDispatch nests
    PROF_SK_Lock,        //           - EngineLock acquisition WAIT inside RunResolvedCommandList (contention with the dispatch hook)
    PROF_SK_Row,         //         - the SKPASS row: PipeState reads, GetVarNs x3, GetIniParams, MorphDump record, log
    PROF_DI_PortSect,    //     - PortRunSection: THEIR ini command list for this component (inside Inject)
    PROF_DI_PsLookup,    //       - RunSectionByName: the fq name -> section lookup (Widen + map find), per draw
    PROF_DI_PsDraw,      //       - the section's drawindexed callback: the addon's injected draw (hk.drawindexed)
    PROF_PS_Observe,     //       - TypedTrampoline: registry keys narrowed + the addon's typed hooks, per command
    PROF_PS_Trace,       //       - CommandTrampoline: ini_line narrowed per command (H3 trace / CHAIN log only)
    PROF_DI_ShadowCast,  //     - the whole is_full_shadow_pass cast: N mods x N components per depth target
    PROF_DI_Restore,     //     - restore_state: 5 VBs + IB + bone CBVs, on EVERY claimed draw
    PROF_DI_TexHash,     //     - hash-keyed texture override walk on a claimed draw (bound tables x 128 slots)
    PROF_TH_Walk,        //       - per bound table: heap offset + the 128-slot res/hash resolve (ResolveBoundTextures)
    PROF_TH_Select,      //       - per bound table, its slot loop: selection (TexSelectIdx), LOD alias, learned-slot replay, stamp
    PROF_TexSel_Engine,  //         - SelectTextureOverride for a hash that HAS a section: engine lock + their list run.
                         //           Nests wherever a selection runs (TH:Select, copy hooks)
    PROF_TexSel_Lock,    //           - EngineLock acquisition WAIT in it
    PROF_TH_Detect,      //       - $object_detected = 1 for the claimed mod, then its texture-timing / eager-load notes
    PROF_TH_DetLock,     //         - EngineLock acquisition WAIT, only when the flag is not already 1
    PROF_TH_Prep,        //       - the table walk's setup: descriptor size, bound tables, scan lengths, read slots
    PROF_DI_VbPrep,      //     - VB/IB snapshot, ApplyVB, restore_state setup, cloth/deform/shadow-table binds
    PROF_DI_DrawComp,    //     - draw_comp, one component (claimed draw, or each component of a shadow cast). SELF =
                         //       the section's RAII teardown (EngineScope unlock, DrawListScope, PushCbGuard, PortRestore)
    PROF_DI_CompSetup,   //       - BEFORE PortSect: section name, hooks, EngineScope, PortRestore snapshot,
                         //         CB/VB push, RELOCBIND (DI:ShapeKeys nests here at runtime)
    PROF_DI_EngLock,     //         - EngineScope acquisition WAIT (engine lock, then held across the whole section)
    PROF_CS_Prep,        //         - section name, hooks, the section-draw state: before the lock
    PROF_CS_Restore,     //         - PortRestore snapshot (layout tables, heaps, PSO), DrawListScope
    PROF_CS_Push,        //         - CB / VB pushes, RtPal, bone CB records, shadow bone, shapekey passes
    PROF_CS_Reloc,       //         - relocated VB / IB binds, the MorphDump draw row and the SKCAP vb6 snapshot
    PROF_CS_SectTex,     //         - PublishSectionTextures
    PROF_DI_CompPost,    //       - AFTER PortSect: mid-restore / MorphDump draw rows, CB restore, counters, fallback restore
                         //       (DI:PortSect is a child of DrawComp at runtime; its row sits above, unmoved)
    PROF_DI_Tail,        //     - the legacy tail: MERGEDDRAW, GetDrawOverrides, replacement DrawIndexedInstanced
    PROF_DI_Prologue,    //     - EVERY draw pays this: from the top of Inject to the claimed branch
    PROF_DI_Probe,       //       - the unconditional probe/budget block (g_char_mesh_mtx + set ops)
    PROF_DI_Vb6Reader,   //       - the slot-6 relocation lookup on every draw while debug is on (g_buffer_reloc.Resolve)
    PROF_CmdLock,        //   - g_cmd_mutex acquisition wait (nested inside whatever scope was running)
    PROF_Dispatch,       // on_dispatch                 (shapekey ShaderOverride + the whole cs-u0 chain)
    PROF_CsU0,           //   - cs-u0 resolve + validate + publish (only on shapekey dispatches)
    PROF_Disp_Lock,      //   - EngineLock acquisition WAIT in DispatchShaderOverride (the draw hook calls it too)
    PROF_Disp_Resolve,   //   - SkResolveU0, every dispatch
    PROF_Disp_Ovr,       //   - RunDispatchOverrides (their [ShaderOverride] lookup + sections)
    PROF_Disp_Learn,     //   - LearnSkinCache, every dispatch
    PROF_IssueDispatch,  // MigotoPort::IssueDispatch (their compute executor: CollectBinds/RootSig/PSO/
                         // BindDescriptors/Dispatch). SUB: it always nests in whatever ran the list.
    PROF_IssueDraw,      // MigotoPort::IssueDraw -- the sibling executor for draws their engine issues itself
    PROF_UploadQueue,    //   - QueueUpload / QueueTextureUpload: MakeStaging + Map + memcpy
    PROF_UploadFlush,    //   - FlushPendingUploads: copies + transitions + staging retire
    PROF_Present,        // on_present                  (TOTAL frame-boundary work)
    PROF_DrainTexHash,   //   - texture readback + BLOCKING fence
    PROF_EngineTick,     //   - ModLoader::Tick + the PORT's [Present] command lists + UI + presence tick
    PROF_ET_TexStore,    //     - SubstantiateWclResources: the per-frame walk describing substantiated mod textures
    PROF_ET_Begin,       //     - the [Present] list's Begin: ring fence wait + allocator/list reset
    PROF_ET_Lists,       //     - their [Present] pre + input + post, and the per-mod var reads between
    PROF_ET_Submit,      //     - flush immediate list + pending uploads + back-buffer restore + Close/Execute/Signal
    PROF_ET_Presence,    //     - PresenceTick + RunPendingPost
    PROF_PresTail,       //   - on_present tail: shapekey/ibhash-drain/diag
    PROF_IbHashDrain,    //     - IbHash::DrainQueue (GPU IB-hash anchor)
    PROF_DiagPresent,    //     - DiagOnPresent (debug-only dumps/captures)
    // The hooks that had no bucket: each is a per-call tax the game's recording threads pay.
    PROF_BindVB,        // on_bind_vertex_buffers (VA + width per buffer, from the runtime)
    PROF_BindIB,        // on_bind_index_buffer
    PROF_BindRT,        // on_bind_render_targets
    PROF_PushDesc,      // on_push_descriptors (root CBV/SRV/UAV: view -> resource, VA, width)
    PROF_CmdReset,      // on_reset_command_list
    PROF_CmdClose,      // on_close_command_list (our buffers back to COMMON, removal pin ring)
    PROF_CmdExecute,    // on_execute_command_list (removal pin check + ring)
    PROF_CopyBufRegion, // on_copy_buffer_region (IbHash forget)
    PROF_InitView,      // on_init_resource_view
    PROF_COUNT
};
#ifdef WWMI_DEBUG
inline const char* g_prof_names[PROF_COUNT] = {
    "DescUpdate", "DescCopy", "CopyBufToTex", "  CB:Alloc", "  CB:Copy", "  CB:Crc", "  CB:Map", "  CB:TexLod", "CopyTexRegion", "Barrier", "BindDescTables",
    "BindPipeline", "DrawIndexed", "  DI:TexScan", "  DI:RfxDecide", "  DI:ClaimInject", "    DI:BoneHash",
    "  DI:Inject",
    "    DI:ShapeKeys", "      SK:OwnBuf", "      SK:Lists",
    "        SK:Cb0", "        SK:Run", "          SK:Lock", "        SK:Row", "    DI:PortSect",
    "      DI:PsLookup", "      DI:PsDraw", "      PS:Observe", "      PS:Trace",
    "    DI:ShadowCast", "    DI:Restore",
    "    DI:TexHash", "      TH:Walk", "      TH:Select", "        TexSel:Engine", "          TexSel:Lock",
    "      TH:Detect", "        TH:DetLock", "      TH:Prep",
    "    DI:VbPrep", "    DI:DrawComp", "      DI:CompSetup", "        DI:EngLock",
    "        CS:Prep", "        CS:Restore", "        CS:Push", "        CS:Reloc", "        CS:SectTex", "      DI:CompPost",
    "    DI:Tail",
    "    DI:Prologue", "      DI:Probe", "      DI:Vb6Reader",
    "  CmdLock:wait",
    "Dispatch", "  CsU0", "  Disp:Lock", "  Disp:Resolve", "  Disp:Ovr", "  Disp:Learn", "IssueDispatch", "IssueDraw", "  UploadQueue", "  UploadFlush",
    "Present(total)", "  DrainTexHash", "  EngineTick", "    ET:TexStore", "    ET:Begin", "    ET:Lists", "    ET:Submit", "    ET:Presence", "  PresTail",
    "    PT:IbHashDrain", "    PT:DiagPresent",
    "BindVB", "BindIB", "BindRT", "PushDesc", "CmdReset", "CmdClose", "CmdExecute", "CopyBufRegion", "InitView"
};
static_assert(sizeof(g_prof_names) / sizeof(g_prof_names[0]) == PROF_COUNT,
    "g_prof_names is out of sync with ProfBucket -- rows would be MISLABELLED, which is worse "
    "than a missing row: every reading after the gap names the wrong step.");

static_assert(PROF_COUNT <= kProfMaxBuckets, "prof_core.h's accumulators are smaller than the bucket list.");

// The lib's steps (prof_core.h) bound to their rows. Once, at AddonInit, before the profiler can be enabled.
inline void ProfBindLibSteps()
{
    g_prof_lib_bucket[PL_UploadQueue] = PROF_UploadQueue;
    g_prof_lib_bucket[PL_UploadFlush] = PROF_UploadFlush;
    g_prof_lib_bucket[PL_IssueDraw] = PROF_IssueDraw;
    g_prof_lib_bucket[PL_IssueDispatch] = PROF_IssueDispatch;
    g_prof_lib_bucket[PL_SK_Lock] = PROF_SK_Lock;
    g_prof_lib_bucket[PL_Disp_Lock] = PROF_Disp_Lock;
    g_prof_lib_bucket[PL_DI_PsLookup] = PROF_DI_PsLookup;
    g_prof_lib_bucket[PL_PS_Trace] = PROF_PS_Trace;
    g_prof_lib_bucket[PL_PS_Observe] = PROF_PS_Observe;
    g_prof_lib_bucket[PL_TexSel_Engine] = PROF_TexSel_Engine;
    g_prof_lib_bucket[PL_TexSel_Lock] = PROF_TexSel_Lock;
}

// The busiest thread's gross, published for the overlay the same way the summed total is.
inline double        g_prof_disp_thread_max_ms = 0.0;
inline uint32_t      g_prof_disp_thread_max_tid = 0;
inline int           g_prof_disp_thread_count = 0;
inline constexpr int kProfHotRows = 8;
inline double        g_prof_disp_thr_ms[kProfThreads] = {};
inline int           g_prof_disp_thr_n = 0;
inline int           g_prof_disp_hot_bucket[kProfHotRows] = {}; // index into g_prof_names
inline double        g_prof_disp_hot_ms[kProfHotRows] = {};     // that bucket's SELF ms on the busiest thread
inline int           g_prof_disp_hot_n = 0;

inline double   g_prof_disp_avg_ms[PROF_COUNT] = {};
inline double   g_prof_disp_self_ms[PROF_COUNT] = {}; // exclusive
inline double   g_prof_disp_max_ms[PROF_COUNT] = {};
inline uint32_t g_prof_disp_calls[PROF_COUNT] = {}; // avg calls/frame over the window
inline double   g_prof_disp_fr_avg = 0.0;           // frame time (ms) avg/min/max over the window
inline double   g_prof_disp_fr_min = 0.0;
inline double   g_prof_disp_fr_max = 0.0;
inline double   g_prof_disp_fps = 0.0;
inline int      g_prof_disp_frames = 0;         // frames counted in the window
inline double   g_prof_disp_overhead_ms = 0.0;  // est. profiling overhead per frame (ms)
inline uint32_t g_prof_disp_total_calls = 0;    // total ProfScopes/frame (all buckets)
inline double   g_prof_disp_total_ms = 0.0;     // sum of top-level hooks (gross, incl. timer overhead)
inline double   g_prof_disp_total_net_ms = 0.0; // gross - overhead

inline std::atomic<uint64_t> g_th_all{ 0 };
inline std::atomic<uint64_t> g_th_rehash{ 0 };
inline std::atomic<uint64_t> g_th_same{ 0 };
inline std::atomic<uint64_t> g_th_bytes{ 0 };
inline void                  ProfVolAdd(ProfVol v, uint32_t n)
{
    g_prof_vol_t[ProfThreadSlot()][v].fetch_add(n, std::memory_order_relaxed);
}
inline uint32_t g_prof_disp_dc_copies = 0;
inline uint32_t g_prof_disp_dc_descs = 0;
inline uint32_t g_prof_disp_dc_swaps = 0;

struct ProfMem {
    uint64_t vram_budget = 0; // IDXGIAdapter3 LOCAL
    uint64_t vram_usage = 0;
    uint64_t host_budget = 0; // IDXGIAdapter3 NON_LOCAL (upload + spill)
    uint64_t host_usage = 0;
    uint64_t proc_private = 0; // PROCESS_MEMORY_COUNTERS_EX
    uint64_t proc_ws = 0;
    uint32_t n_va_to_res = 0;
    uint32_t n_res_to_va = 0;
    uint32_t n_cmd_state = 0;
    uint32_t n_sk_pending = 0;
    uint32_t n_geomods = 0;
    uint32_t n_heap_maps = 0;
    uint32_t n_heap_rejected = 0;
    uint32_t n_heap_destroyed = 0;
    uint64_t heap_map_bytes = 0;
    uint32_t staging_used = 0;
    uint32_t staging_cap = 0;
    uint32_t staging_freed = 0;
    uint32_t n_rootsigs = 0;
    uint32_t n_psos = 0;
};
inline ProfMem g_prof_mem{};      // latest sample
inline ProfMem g_prof_mem_base{}; // first sample of the session (baseline for growth)
inline ProfMem g_prof_mem_peak{}; // per-field maxima
inline bool    g_prof_mem_have = false;
inline double  g_prof_mem_age_s = 0.0; // seconds since baseline -- growth is meaningless without it

inline void ProfMemFold(const ProfMem& s, double age_s)
{
    g_prof_mem = s;
    g_prof_mem_age_s = age_s;
    if (!g_prof_mem_have) {
        g_prof_mem_base = s;
        g_prof_mem_peak = s;
        g_prof_mem_have = true;
        return;
    }
    ProfMem& p = g_prof_mem_peak;
    auto     mx64 = [](uint64_t& a, uint64_t b) { if (b > a) a = b; };
    auto     mx32 = [](uint32_t& a, uint32_t b) { if (b > a) a = b; };
    mx64(p.vram_usage, s.vram_usage);
    mx64(p.vram_budget, s.vram_budget);
    mx64(p.host_usage, s.host_usage);
    mx64(p.host_budget, s.host_budget);
    mx64(p.proc_private, s.proc_private);
    mx64(p.proc_ws, s.proc_ws);
    mx32(p.n_va_to_res, s.n_va_to_res);
    mx32(p.n_res_to_va, s.n_res_to_va);
    mx32(p.n_heap_maps, s.n_heap_maps);
    mx64(p.heap_map_bytes, s.heap_map_bytes);
    mx32(p.n_heap_rejected, s.n_heap_rejected);
    mx32(p.n_heap_destroyed, s.n_heap_destroyed);
    mx32(p.n_cmd_state, s.n_cmd_state);
    mx32(p.n_sk_pending, s.n_sk_pending);
    mx32(p.n_geomods, s.n_geomods);
    mx32(p.staging_used, s.staging_used);
    mx32(p.staging_freed, s.staging_freed);
    mx32(p.n_rootsigs, s.n_rootsigs);
    mx32(p.n_psos, s.n_psos);
}

// DUMP button: capture ~N seconds of running-mean stats to a text file (overlay button sets the request).
inline std::atomic<bool> g_prof_dump_request{ false };
inline double            g_prof_dump_secs = 5.0;
inline char              g_prof_dump_last[300] = "";  // last written path (shown in overlay)
inline std::atomic<bool> g_prof_dump_active{ false }; // true while a dump session is accumulating

inline void ProfFrameEnd()
{
    if (g_prof_enabled && g_prof_percall_ns == 0.0) ProfCalibrate(); // one-time, first enabled frame
    HotPath::FrameEnd(g_prof_dump_active.load());
    const int64_t  freq = prof_qpc_freq();
    const int64_t  now = prof_qpc_now();
    static int64_t last = now;
    const double   frame_ms = (now - last) * 1000.0 / (double)freq;
    last = now;

    double        fms[PROF_COUNT] = {};
    double        fself[PROF_COUNT] = {};
    double        fcalls[PROF_COUNT] = {};
    static double ftms[kProfThreads][PROF_COUNT] = {};   // inclusive
    static double ftself[kProfThreads][PROF_COUNT] = {}; // exclusive
    static double ft_gross[kProfThreads] = {};
    const int     n_thr = g_prof_thread_n.load(std::memory_order_relaxed);
    double        f_dcc = 0.0;
    double        f_dcd = 0.0;
    double        f_dcs = 0.0;
    for (int t = 0; t < n_thr; ++t) {
        double g = 0.0;
        for (int i = 0; i < PROF_COUNT; ++i) {
            ftms[t][i] = g_prof_ns_t[t][i].exchange(0, std::memory_order_relaxed) / 1000000.0;
            ftself[t][i] = g_prof_self_ns_t[t][i].exchange(0, std::memory_order_relaxed) / 1000000.0;
            fms[i] += ftms[t][i];
            fself[i] += ftself[t][i];
            fcalls[i] += (double)g_prof_calls_t[t][i].exchange(0, std::memory_order_relaxed);
            g += ftself[t][i];
        }
        ft_gross[t] = g;
        f_dcc += (double)g_prof_vol_t[t][kProfVolDcCopies].exchange(0, std::memory_order_relaxed);
        f_dcd += (double)g_prof_vol_t[t][kProfVolDcDescs].exchange(0, std::memory_order_relaxed);
        f_dcs += (double)g_prof_vol_t[t][kProfVolDcSwaps].exchange(0, std::memory_order_relaxed);
    }
    uint64_t f_gt_ns[GT_COUNT];
    uint32_t f_gt_n[GT_COUNT];
    for (int k = 0; k < GT_COUNT; ++k) {
        f_gt_ns[k] = g_gt_ns[k].exchange(0, std::memory_order_relaxed);
        f_gt_n[k] = g_gt_n[k].exchange(0, std::memory_order_relaxed);
    }
    const uint64_t f_gt_frame_ns = g_gt_frame_ns.exchange(0, std::memory_order_relaxed);
    const uint32_t f_gt_frame_n = g_gt_frame_n.exchange(0, std::memory_order_relaxed);
    const uint32_t f_gt_regions = g_gt_frames_read.exchange(0, std::memory_order_relaxed);

    static int64_t win_start = now;
    static int     win_frames = 0;
    static double  win_fr_mean = 0.0;
    static double  win_fr_min = 1e30;
    static double  win_fr_max = 0.0;
    static double  win_mean[PROF_COUNT] = {};
    static double  win_max[PROF_COUNT] = {};
    static double  win_self_mean[PROF_COUNT] = {};
    static double  win_calls_mean[PROF_COUNT] = {};
    static double  win_dcc = 0.0;
    static double  win_dcd = 0.0;
    static double  win_dcs = 0.0;

    ++win_frames;
    const double inv_n = 1.0 / (double)win_frames;
    win_fr_mean += (frame_ms - win_fr_mean) * inv_n;
    if (frame_ms < win_fr_min) win_fr_min = frame_ms;
    if (frame_ms > win_fr_max) win_fr_max = frame_ms;
    for (int i = 0; i < PROF_COUNT; ++i) {
        win_mean[i] += (fms[i] - win_mean[i]) * inv_n;
        win_self_mean[i] += (fself[i] - win_self_mean[i]) * inv_n;
        win_calls_mean[i] += (fcalls[i] - win_calls_mean[i]) * inv_n;
        if (fms[i] > win_max[i]) win_max[i] = fms[i];
    }
    win_dcc += (f_dcc - win_dcc) * inv_n;
    win_dcd += (f_dcd - win_dcd) * inv_n;
    win_dcs += (f_dcs - win_dcs) * inv_n;
    static double win_t_gross[kProfThreads] = {};
    static double win_t_self[kProfThreads][PROF_COUNT] = {}; // for the overlay's hot-path rows
    for (int t = 0; t < n_thr; ++t) {
        win_t_gross[t] += (ft_gross[t] - win_t_gross[t]) * inv_n;
        for (int i = 0; i < PROF_COUNT; ++i)
            win_t_self[t][i] += (ftself[t][i] - win_t_self[t][i]) * inv_n;
    }

    if (now - win_start >= freq && win_frames > 0) { // ~1s elapsed -> publish + reset
        const double secs = (now - win_start) / (double)freq;
        g_prof_disp_frames = win_frames;
        g_prof_disp_fps = win_frames / secs;
        g_prof_disp_fr_avg = win_fr_mean;
        g_prof_disp_fr_min = (win_fr_min >= 1e30) ? 0.0 : win_fr_min;
        g_prof_disp_fr_max = win_fr_max;
        double gross = 0.0;
        double call_total = 0.0;
        for (int i = 0; i < PROF_COUNT; ++i) {
            g_prof_disp_avg_ms[i] = win_mean[i]; // running mean -> already per-frame
            g_prof_disp_self_ms[i] = win_self_mean[i];
            g_prof_disp_max_ms[i] = win_max[i];
            g_prof_disp_calls[i] = (uint32_t)(win_calls_mean[i] + 0.5);
            gross += win_self_mean[i];       // EXCLUSIVE sum = every nanosecond counted exactly once
            call_total += win_calls_mean[i]; // avg scopes/frame (each carries timer overhead)
        }
        // Self-profiling: estimated overhead the timers themselves add per frame, and the addon's true cost net of it.
        g_prof_disp_total_calls = (uint32_t)(call_total + 0.5);
        g_prof_disp_overhead_ms = call_total * g_prof_percall_ns / 1000000.0;
        g_prof_disp_total_ms = gross;
        g_prof_disp_total_net_ms = gross - g_prof_disp_overhead_ms;
        if (g_prof_disp_total_net_ms < 0.0) g_prof_disp_total_net_ms = 0.0;
        g_prof_disp_dc_copies = (uint32_t)(win_dcc + 0.5);
        g_prof_disp_dc_descs = (uint32_t)(win_dcd + 0.5);
        g_prof_disp_dc_swaps = (uint32_t)(win_dcs + 0.5);
        {
            double mx = 0.0;
            int    mi = -1;
            int    ord[kProfThreads];
            int    nn = 0;
            for (int t = 0; t < n_thr; ++t) {
                if (win_t_gross[t] > 0.005) ord[nn++] = t;
                if (win_t_gross[t] > mx) {
                    mx = win_t_gross[t];
                    mi = t;
                }
            }
            for (int a = 0; a < nn; ++a) // selection sort, nn <= 24
                for (int b2 = a + 1; b2 < nn; ++b2)
                    if (win_t_gross[ord[b2]] > win_t_gross[ord[a]]) {
                        int tmp = ord[a];
                        ord[a] = ord[b2];
                        ord[b2] = tmp;
                    }
            g_prof_disp_thr_n = (nn < kProfThreads) ? nn : kProfThreads;
            for (int k = 0; k < g_prof_disp_thr_n; ++k) g_prof_disp_thr_ms[k] = win_t_gross[ord[k]];
            g_prof_disp_thread_max_ms = mx;
            g_prof_disp_thread_max_tid = (mi >= 0) ? g_prof_tid[mi].load(std::memory_order_relaxed) : 0;
            g_prof_disp_thread_count = n_thr;
            // ... and where that thread's own time went (largest SELF times, which add up to its gross)
            g_prof_disp_hot_n = 0;
            if (mi >= 0) {
                int shown[kProfHotRows];
                int ns = 0;
                for (int slot = 0; slot < kProfHotRows; ++slot) {
                    int    best = -1;
                    double bv = 0.005;
                    for (int i = 0; i < PROF_COUNT; ++i) {
                        bool already = false;
                        for (int p = 0; p < ns; ++p)
                            if (shown[p] == i) {
                                already = true;
                                break;
                            }
                        if (!already && win_t_self[mi][i] > bv) {
                            bv = win_t_self[mi][i];
                            best = i;
                        }
                    }
                    if (best < 0) break;
                    shown[ns++] = best;
                    g_prof_disp_hot_bucket[slot] = best;
                    g_prof_disp_hot_ms[slot] = bv;
                    g_prof_disp_hot_n = slot + 1;
                }
            }
        }
        win_start = now;
        win_frames = 0;
        win_fr_mean = 0.0;
        win_fr_min = 1e30;
        win_fr_max = 0.0;
        for (int i = 0; i < PROF_COUNT; ++i) {
            win_mean[i] = 0.0;
            win_self_mean[i] = 0.0;
            win_max[i] = 0.0;
            win_calls_mean[i] = 0.0;
        }
        win_dcc = 0.0;
        win_dcd = 0.0;
        win_dcs = 0.0;
        for (int t = 0; t < kProfThreads; ++t) {
            win_t_gross[t] = 0.0;
            for (int i = 0; i < PROF_COUNT; ++i) win_t_self[t][i] = 0.0;
        }
    }

    // ---- multi-second DUMP session (overlay button) -> averaged stats to a text file ----
    static bool                   dmp = false;
    static int64_t                dmp_start = 0;
    static int                    dmp_frames = 0;
    static double                 dmp_fr_mean = 0.0;
    static double                 dmp_fr_min = 1e30;
    static double                 dmp_fr_max = 0.0;
    static double                 dmp_mean[PROF_COUNT] = {};
    static double                 dmp_max[PROF_COUNT] = {};
    static double                 dmp_calls[PROF_COUNT] = {};
    static double                 dmp_self[PROF_COUNT] = {};
    static double                 dmp_dcc = 0.0;
    static double                 dmp_dcd = 0.0;
    static double                 dmp_dcs = 0.0;
    static uint64_t               dmp_th0[4] = {};
    static double                 dmp_t_mean[kProfThreads][PROF_COUNT] = {}; // per thread, per bucket, INCLUSIVE
    static double                 dmp_t_self[kProfThreads][PROF_COUNT] = {}; // ... and EXCLUSIVE
    static PortApi::GpuWorkTotals dmp_gw0;                                   // GPU WORK: totals at session start
    static uint64_t               dmp_gwn0[PGW_COUNT] = {};
    static uint64_t               dmp_gwb0[PGW_COUNT] = {};
    static bool                   dmp_debug_was = false; // restored when the session ends
    static uint32_t               dmp_hist[41] = {};     // frame ms, 1 ms bins, last = 40+
    static uint64_t               dmp_gt_ns[GT_COUNT] = {};
    static uint64_t               dmp_gt_frame_ns = 0;
    static uint32_t               dmp_gt_n[GT_COUNT] = {};
    static uint32_t               dmp_gt_frame_n = 0;
    static uint32_t               dmp_gt_regions = 0;
    static uint32_t               dmp_gt_dropped0 = 0;
    if (g_prof_dump_request.exchange(false)) { // (re)start a session
        if (!dmp) {
            dmp_debug_was = g_debug;
            if (g_debug) SetDiagnostics(false, g_log_verbosity);
        }
        dmp = true;
        g_prof_dump_active.store(true);
        dmp_start = now;
        dmp_frames = 0;
        HotPath::SessionStart();
        PortApi::CommandCostMark();
        PortApi::GetGpuWorkTotals(&dmp_gw0);
        for (int k = 0; k < PGW_COUNT; ++k) {
            dmp_gwn0[k] = g_gw_n[k].load(std::memory_order_relaxed);
            dmp_gwb0[k] = g_gw_bytes[k].load(std::memory_order_relaxed);
        }
        dmp_fr_mean = 0.0;
        dmp_fr_min = 1e30;
        dmp_fr_max = 0.0;
        for (int i = 0; i < PROF_COUNT; ++i) {
            dmp_mean[i] = 0.0;
            dmp_self[i] = 0.0;
            dmp_max[i] = 0.0;
            dmp_calls[i] = 0.0;
        }
        dmp_dcc = 0.0;
        dmp_dcd = 0.0;
        dmp_dcs = 0.0;
        dmp_th0[0] = g_th_all.load();
        dmp_th0[1] = g_th_rehash.load();
        dmp_th0[2] = g_th_same.load();
        dmp_th0[3] = g_th_bytes.load();
        for (int t = 0; t < kProfThreads; ++t)
            for (int i = 0; i < PROF_COUNT; ++i) {
                dmp_t_mean[t][i] = 0.0;
                dmp_t_self[t][i] = 0.0;
            }
        for (int k = 0; k < GT_COUNT; ++k) {
            dmp_gt_ns[k] = 0;
            dmp_gt_n[k] = 0;
        }
        dmp_gt_frame_ns = 0;
        dmp_gt_frame_n = 0;
        dmp_gt_regions = 0;
        dmp_gt_dropped0 = g_gt_dropped.load(std::memory_order_relaxed);
        for (int k = 0; k < 41; ++k) dmp_hist[k] = 0;
    }
    if (dmp) {
        ++dmp_frames;
        const double di = 1.0 / (double)dmp_frames;
        dmp_fr_mean += (frame_ms - dmp_fr_mean) * di;
        ++dmp_hist[frame_ms < 0.0 ? 0 : frame_ms >= 40.0 ? 40
                                                         : (int)frame_ms];
        if (frame_ms < dmp_fr_min) dmp_fr_min = frame_ms;
        if (frame_ms > dmp_fr_max) dmp_fr_max = frame_ms;
        for (int i = 0; i < PROF_COUNT; ++i) {
            dmp_mean[i] += (fms[i] - dmp_mean[i]) * di;
            dmp_self[i] += (fself[i] - dmp_self[i]) * di;
            dmp_calls[i] += (fcalls[i] - dmp_calls[i]) * di;
            if (fms[i] > dmp_max[i]) dmp_max[i] = fms[i];
        }
        dmp_dcc += (f_dcc - dmp_dcc) * di;
        dmp_dcd += (f_dcd - dmp_dcd) * di;
        dmp_dcs += (f_dcs - dmp_dcs) * di;
        for (int k = 0; k < GT_COUNT; ++k) {
            dmp_gt_ns[k] += f_gt_ns[k];
            dmp_gt_n[k] += f_gt_n[k];
        }
        dmp_gt_frame_ns += f_gt_frame_ns;
        dmp_gt_frame_n += f_gt_frame_n;
        dmp_gt_regions += f_gt_regions;
        for (int t = 0; t < n_thr; ++t)
            for (int i = 0; i < PROF_COUNT; ++i) {
                dmp_t_mean[t][i] += (ftms[t][i] - dmp_t_mean[t][i]) * di;
                dmp_t_self[t][i] += (ftself[t][i] - dmp_t_self[t][i]) * di;
            }
        if ((now - dmp_start) >= (int64_t)(g_prof_dump_secs * (double)freq) && dmp_frames > 0) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            char path[300];
            std::snprintf(path, sizeof(path), "wwmi_dx12_prof_%04d%02d%02d_%02d%02d%02d.txt",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
            FILE* f = nullptr;
            fopen_s(&f, path, "w");
            if (f) {
                const double dsecs = (now - dmp_start) / (double)freq;
                double       total_self = 0.0;
                double       call_total = 0.0;
                for (int i = 0; i < PROF_COUNT; ++i) {
                    total_self += dmp_self[i];
                    call_total += dmp_calls[i];
                }
                const double ovh = call_total * g_prof_percall_ns / 1000000.0;
                std::fprintf(f, "WWMI-DX12 profiler dump -- %.1fs, %d frames (%.1f fps), frame avg %.2f ms (min %.2f max %.2f)\n",
                    dsecs, dmp_frames, dmp_frames / dsecs, dmp_fr_mean, (dmp_fr_min >= 1e30 ? 0.0 : dmp_fr_min), dmp_fr_max);
                std::fprintf(f, "debug and logging: OFF for the dump%s\n", dmp_debug_was ? " (were on; turned back on after it)" : "");
                std::fprintf(f, "frame time histogram, 1 ms bins (ms: frames):");
                for (int k = 0; k < 41; ++k)
                    if (dmp_hist[k]) std::fprintf(f, "  %d%s: %u", k, k == 40 ? "+" : "", dmp_hist[k]);
                std::fprintf(f, "\n");
                std::fprintf(f, "per-scope overhead %.1f ns over %.0f scopes/frame; est profiling overhead %.3f ms/frame\n",
                    g_prof_percall_ns, call_total, ovh);
                std::fprintf(f, "ADDON CPU, SUMMED OVER ALL THREADS: %.3f ms/frame (sum of the self column), net of\n"
                                "the timers %.3f ms. This is NOT the frame cost -- the hooks run on the game's own\n"
                                "recording threads, so read the PER-THREAD section below for what the frame pays.\n\n",
                    total_self, (total_self - ovh < 0.0 ? 0.0 : total_self - ovh));
                std::fprintf(f, "%-18s %9s %7s %9s %7s %9s %9s %9s\n",
                    "step", "incl ms", "incl%", "self ms", "self%", "us/call", "max ms", "calls/f");
                for (int i = 0; i < PROF_COUNT; ++i) {
                    const double ipct = dmp_fr_mean > 0.0 ? 100.0 * dmp_mean[i] / dmp_fr_mean : 0.0;
                    const double spct = dmp_fr_mean > 0.0 ? 100.0 * dmp_self[i] / dmp_fr_mean : 0.0;
                    const double per = dmp_calls[i] > 0.0 ? dmp_mean[i] * 1000.0 / dmp_calls[i] : 0.0;
                    std::fprintf(f, "%-18s %9.3f %6.1f%% %9.3f %6.1f%% %9.2f %9.3f %9u\n",
                        g_prof_names[i], dmp_mean[i], ipct, dmp_self[i], spct, per, dmp_max[i],
                        (uint32_t)(dmp_calls[i] + 0.5));
                }
                std::fprintf(f, "\nDescCopy volume/frame: %.0f copies, %.0f descriptors, %.0f swaps\n", dmp_dcc, dmp_dcd, dmp_dcs);
                {
                    const double inv_r = dmp_gt_regions ? 1.0 / (double)dmp_gt_regions : 0.0;
                    std::fprintf(f, "\nGPU ms / frame -- timestamp pairs around what the addon RECORDS, read back %u frame(s)"
                                    " (%u dropped unread).\nIndented rows are inside the row above them; the two hook rows"
                                    " span the hook's first to last recorded command, so\nthe barriers and copies between"
                                    " them, and the pipeline drains they cost, are in the hook row and not in its children.\n",
                        dmp_gt_regions, g_gt_dropped.load(std::memory_order_relaxed) - dmp_gt_dropped0);
                    std::fprintf(f, "  %-28s %9s %9s %10s\n", "bracket", "ms/frame", "per frame", "us each");
                    for (int k = 0; k < GT_COUNT; ++k) {
                        const double ms = (double)dmp_gt_ns[k] / 1e6 * inv_r;
                        const double n = (double)dmp_gt_n[k] * inv_r;
                        std::fprintf(f, "  %-28s %9.3f %9.1f %10.1f\n", g_gt_names[k], ms, n,
                            dmp_gt_n[k] ? (double)dmp_gt_ns[k] / 1e3 / (double)dmp_gt_n[k] : 0.0);
                    }
                    std::fprintf(f, "  GPU frame period ([Present] list start to start): %.2f ms over %u pair(s); CPU frame avg %.2f ms\n",
                        dmp_gt_frame_n ? (double)dmp_gt_frame_ns / 1e6 / (double)dmp_gt_frame_n : 0.0,
                        dmp_gt_frame_n, dmp_fr_mean);
                }
                std::fprintf(f, "Texture mip0 hashes over the dump: %llu, of them onto an already-hashed resource %llu"
                                " (same hash again %llu); %.1f MB hashed\n",
                    (unsigned long long)(g_th_all.load() - dmp_th0[0]), (unsigned long long)(g_th_rehash.load() - dmp_th0[1]),
                    (unsigned long long)(g_th_same.load() - dmp_th0[2]), (g_th_bytes.load() - dmp_th0[3]) / (1024.0 * 1024.0));
                {
                    PortApi::GpuWorkTotals g1;
                    PortApi::GetGpuWorkTotals(&g1);
                    const double inv = 1.0 / (double)dmp_frames;
                    auto         pf = [&](unsigned a1, unsigned a0) { return (double)(unsigned)(a1 - a0) * inv; };
                    std::fprintf(f, "\nGPU WORK / frame -- their counters, the DX11 overlay's \"GPU Performance Impacting Stats\"\n"
                                    "(same code, CommandList.cpp; unsynchronised, so a guideline):\n");
                    std::fprintf(f, "  IniParams GPU resource updates:    %8.1f\n", pf(g1.iniparams_updates, dmp_gw0.iniparams_updates));
                    std::fprintf(f, "  Full resource copies:              %8.1f\n", pf(g1.full_copies, dmp_gw0.full_copies));
                    std::fprintf(f, "  By-Reference resource copies:      %8.1f   (binds and `ref`; no GPU copy on DX11)\n",
                        pf(g1.reference_copies, dmp_gw0.reference_copies));
                    std::fprintf(f, "  Region buffer copies:              %8.1f\n", pf(g1.region_copies, dmp_gw0.region_copies));
                    std::fprintf(f, "  Inter-device resource copies:      %8.1f\n", pf(g1.inter_device_copies, dmp_gw0.inter_device_copies));
                    std::fprintf(f, "  MSAA resources resolved:           %8.1f\n", pf(g1.msaa_resolutions, dmp_gw0.msaa_resolutions));
                    std::fprintf(f, "  Resources cleared:                 %8.1f\n", pf(g1.views_cleared, dmp_gw0.views_cleared));
                    std::fprintf(f, "  Resources [re]created:             %8.1f   (%u over the window)\n",
                        pf(g1.resources_created, dmp_gw0.resources_created),
                        (unsigned)(g1.resources_created - dmp_gw0.resources_created));
                    std::fprintf(f, "  Resource pool swaps:               %8.1f\n", pf(g1.pool_swaps, dmp_gw0.pool_swaps));
                    std::fprintf(f, "  max_copies_per_frame exceeded:     %8.1f\n", pf(g1.max_copies_exceeded, dmp_gw0.max_copies_exceeded));
                    std::fprintf(f, "  Injected draw/dispatch calls:      %8.1f\n", pf(g1.injected_draws, dmp_gw0.injected_draws));
                    std::fprintf(f, "  max_executions_per_frame exceeded: %8.1f\n", pf(g1.max_executions_exceeded, dmp_gw0.max_executions_exceeded));
                    std::fprintf(f, "  Skipped draw calls:                     n/a   (their counter lives in HackerContext, shimmed away)\n");
                    std::fprintf(f, "SECTION REPLAY / frame -- a list run from its recorded trace, run by theirs while recording, or not replayable:\n");
                    std::fprintf(f, "  section lists:             replayed %8.1f  recorded %8.1f  plain %8.1f\n",
                        pf(g1.replay_top_replayed, dmp_gw0.replay_top_replayed),
                        pf(g1.replay_top_recorded, dmp_gw0.replay_top_recorded),
                        pf(g1.replay_top_plain, dmp_gw0.replay_top_plain));
                    std::fprintf(f, "  `run = CommandList` lists: replayed %8.1f  recorded %8.1f  plain %8.1f\n",
                        pf(g1.replay_sub_replayed, dmp_gw0.replay_sub_replayed),
                        pf(g1.replay_sub_recorded, dmp_gw0.replay_sub_recorded),
                        pf(g1.replay_sub_plain, dmp_gw0.replay_sub_plain));
                    PortApi::CommandCostReport(f, dmp_frames);
                    static const char* const kGwNames[PGW_COUNT] = {
                        "shim buffer copies (CopyResource/CopySubresourceRegion)",
                        "shim texture copies",
                        "upload flushes, buffer (Unmap/UpdateSubresource/init)",
                        "upload flushes, texture",
                        "shapekey chain copies (store, relocation)",
                        "views created by the shim (one descriptor each)",
                        "port transitions: barriers recorded",
                        "port transitions: taken by promotion, no barrier",
                        "texture uploads folded into one still queued (IniParams)",
                    };
                    const bool gw_bytes[PGW_COUNT] = { true, false, true, true, true, false, false, false, false };
                    std::fprintf(f, "RECORDED ON D3D12 / frame -- what the GPU was actually asked to do%s:\n",
                        g_prof_enabled ? "" : " (PROFILER WAS OFF: these read 0)");
                    for (int k = 0; k < PGW_COUNT; ++k) {
                        const double n = (double)(g_gw_n[k].load(std::memory_order_relaxed) - dmp_gwn0[k]) * inv;
                        if (gw_bytes[k]) {
                            const double mb = (double)(g_gw_bytes[k].load(std::memory_order_relaxed) - dmp_gwb0[k]) * inv / (1024.0 * 1024.0);
                            std::fprintf(f, "  %-56s %8.1f  %8.3f MB\n", kGwNames[k], n, mb);
                        } else {
                            std::fprintf(f, "  %-56s %8.1f\n", kGwNames[k], n);
                        }
                    }
                    std::fprintf(f, "  (each upload flush and shim copy also records up to 2 barriers. NOT counted: frame-analysis\n"
                                    "   readback, IbHash's own queue, the game's own copies.)\n");
                }
                {
                    double tg[kProfThreads] = {};
                    int    order[kProfThreads];
                    int    n = 0;
                    for (int t = 0; t < n_thr; ++t) {
                        double g = 0.0;
                        for (int i = 0; i < PROF_COUNT; ++i) g += dmp_t_self[t][i]; // exclusive: counted once
                        tg[t] = g;
                        if (g > 0.001) order[n++] = t;
                    }
                    for (int a = 0; a < n; ++a) // selection sort by gross, n <= 24
                        for (int b2 = a + 1; b2 < n; ++b2)
                            if (tg[order[b2]] > tg[order[a]]) {
                                int tmp = order[a];
                                order[a] = order[b2];
                                order[b2] = tmp;
                            }
                    std::fprintf(f, "\nPER-THREAD ms/frame, EXCLUSIVE (%d thread(s) with timed work). The frame pays\n"
                                    "the BUSIEST row, not the sum: the hooks run on the game's own recording threads,\n"
                                    "so only what one thread does serially can lengthen the frame. Each row's buckets\n"
                                    "are that thread's largest SELF times and add up to its gross.\n",
                        n);
                    for (int k = 0; k < n; ++k) {
                        const int      t = order[k];
                        const uint32_t tid = g_prof_tid[t].load(std::memory_order_relaxed);
                        char           tidbuf[32];
                        if (tid == 0xFFFFFFFFu)
                            std::snprintf(tidbuf, sizeof(tidbuf), "OTHER");
                        else
                            std::snprintf(tidbuf, sizeof(tidbuf), "%u", tid);
                        std::fprintf(f, "  tid %-8s gross %7.3f\n", tidbuf, tg[t]);
                        // every SELF time of 0.01 ms or more, largest first -- where this thread's own milliseconds went
                        int shown[PROF_COUNT];
                        int n_shown = 0;
                        for (;;) {
                            int    best = -1;
                            double bv = 0.01;
                            for (int i = 0; i < PROF_COUNT; ++i) {
                                bool already = false;
                                for (int p = 0; p < n_shown; ++p)
                                    if (shown[p] == i) {
                                        already = true;
                                        break;
                                    }
                                if (already) continue;
                                if (dmp_t_self[t][i] > bv) {
                                    bv = dmp_t_self[t][i];
                                    best = i;
                                }
                            }
                            if (best < 0) break;
                            shown[n_shown++] = best;
                            const char* nm = g_prof_names[best];
                            while (*nm == ' ') ++nm; // the display indent is not wanted here
                            std::fprintf(f, "%s    %-22s %7.3f%s", (n_shown % 4 == 1) ? "   " : "", nm, bv,
                                (n_shown % 4 == 0) ? "\n" : "");
                        }
                        if (n_shown % 4) std::fprintf(f, "\n");
                    }
                }
                HotPath::Report(f, dmp_frames);
                if (g_prof_mem_have) {
                    const double MB = 1024.0 * 1024.0;
                    std::fprintf(f, "\nMEMORY (now | baseline | peak | growth over %.0fs since the profiler was enabled)\n",
                        g_prof_mem_age_s);
                    std::fprintf(f, "  VRAM  (LOCAL)     %8.0f MB used / %8.0f MB budget | base %8.0f | peak %8.0f | %+.0f MB\n",
                        g_prof_mem.vram_usage / MB, g_prof_mem.vram_budget / MB,
                        g_prof_mem_base.vram_usage / MB, g_prof_mem_peak.vram_usage / MB,
                        (double)((int64_t)g_prof_mem.vram_usage - (int64_t)g_prof_mem_base.vram_usage) / MB);
                    std::fprintf(f, "  HOST  (NON_LOCAL) %8.0f MB used / %8.0f MB budget | base %8.0f | peak %8.0f | %+.0f MB   <- upload heaps + VRAM spill\n",
                        g_prof_mem.host_usage / MB, g_prof_mem.host_budget / MB,
                        g_prof_mem_base.host_usage / MB, g_prof_mem_peak.host_usage / MB,
                        (double)((int64_t)g_prof_mem.host_usage - (int64_t)g_prof_mem_base.host_usage) / MB);
                    std::fprintf(f, "  process private   %8.0f MB (working set %.0f MB) | base %8.0f | peak %8.0f | %+.0f MB\n",
                        g_prof_mem.proc_private / MB, g_prof_mem.proc_ws / MB,
                        g_prof_mem_base.proc_private / MB, g_prof_mem_peak.proc_private / MB,
                        (double)((int64_t)g_prof_mem.proc_private - (int64_t)g_prof_mem_base.proc_private) / MB);
                    std::fprintf(f, "  our tables: va_to_res %u (base %u) | res_to_va %u (%u) | "
                                    "cmd_state %u (%u) | sk_pending %u | geomods %u\n",
                        g_prof_mem.n_va_to_res, g_prof_mem_base.n_va_to_res,
                        g_prof_mem.n_res_to_va, g_prof_mem_base.n_res_to_va,
                        g_prof_mem.n_cmd_state, g_prof_mem_base.n_cmd_state,
                        g_prof_mem.n_sk_pending, g_prof_mem.n_geomods);
                    std::fprintf(f, "  heap mirrors: %u live descriptor-heap arrays, %.1f MB; %u heap(s) destroyed and retired; "
                                    "%u write(s) to heaps over 2M descriptors left untracked\n",
                        g_prof_mem.n_heap_maps, g_prof_mem.heap_map_bytes / MB, g_prof_mem.n_heap_destroyed,
                        g_prof_mem.n_heap_rejected);
                    std::fprintf(f, "  port heaps: staging %u/%u used (%u on free list, peak %u) | rootsigs %u | PSOs %u\n",
                        g_prof_mem.staging_used, g_prof_mem.staging_cap, g_prof_mem.staging_freed,
                        g_prof_mem_peak.staging_used, g_prof_mem.n_rootsigs, g_prof_mem.n_psos);
                }
                std::fclose(f);
                std::snprintf(g_prof_dump_last, sizeof(g_prof_dump_last), "%s", path);
            }
            dmp = false;
            g_prof_dump_active.store(false);
            if (dmp_debug_was) SetDiagnostics(true, g_log_verbosity);
        }
    }
}
#else
inline void ProfBindLibSteps() {}
inline void ProfVolAdd(ProfVol, uint32_t) {}
inline void ProfFrameEnd() {}
#endif
