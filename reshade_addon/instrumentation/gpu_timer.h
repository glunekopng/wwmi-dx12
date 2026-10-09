#pragma once
#include <d3d12.h>
#include <wrl.h>
#include <atomic>
#include <cstdint>
#include "diag.h"
#include "hot_path.h"

enum GpuTimerBucket {
    GT_Present = 0,     // the whole [Present] list: their chains, uploads, back-buffer restore, mesh hash
    GT_PresentDraw,     //   - a draw their engine issues on it (dx12_pipeline.cpp IssueDraw)
    GT_PresentDispatch, //   - a dispatch their engine issues on it (IssueDispatch): skeleton chains
    GT_IbHash,          //   - the mesh-hash dispatch
    GT_ClaimedDraw,     // the claimed-draw path on the game's list: VB/IB swap, barriers, sections, restore
    GT_SectionDraw,     //   - one injected section draw (draw_section_draw.cpp)
    GT_IssueDraw,       //   - a draw their engine issues on the game's list
    GT_IssueDispatch,   //   - a dispatch their engine issues on the game's list (shapekeys, chains)
    GT_ShapeKeys,       //   - RunShapeKeyPasses: the mod's shapekey chain, copies and dispatches with barriers
    GT_RelocRestore,    //   - the relocated buffers' transitions back to UAV (prep's mirror; one each way)
    GT_DispatchHook,    // the dispatch hook on the game's list: what the dispatch overrides record
    GT_UploadFlush,     // FlushPendingUploads, either list: IniParams and texture uploads with their barriers
    GT_ShimCopyXit,     // the shim's CopyResource: its two transitions (the first stamp waits for prior work)
    GT_ShimCopy,        // the shim's CopyResource / buffer region copy itself, either list
    GT_ShimCopyCs,      // the same copy as the BufCopy compute dispatch (transitions, dispatch, restore)
    GT_COUNT
};
inline const char* const g_gt_names[GT_COUNT] = {
    "Present list", "  on it: IssueDraw", "  on it: IssueDispatch", "  on it: IbHash dispatch",
    "claimed draw (game list)", "  in it: section draw", "  in it: IssueDraw", "  in it: IssueDispatch",
    "  in it: shapekey passes", "  in it: reloc restore",
    "dispatch hook (game list)", "upload flush (either list)", "shim copy transitions", "shim copy itself",
    "shim copy as compute"
};

class GpuTimer {
public:
    static constexpr UINT kRing = 8;     // frame regions kept before one is reused
    static constexpr UINT kPairs = 2048; // timestamp pairs per region
    // Stamp the start of one bracket. Returns the pair, or -1 when off, out of pairs, or not built.
    WWMI_DEBUG_FN int Begin(ID3D12GraphicsCommandList* cl, int bucket) WWMI_DEBUG_BODY(return -1;);
    // Stamp its end and resolve the pair into this frame's region.
    WWMI_DEBUG_FN void End(ID3D12GraphicsCommandList* cl, int pair) WWMI_DEBUG_BODY();
    // After the present-pass submit: the region is stamped with that fence value and the next one opens.
    WWMI_DEBUG_FN void FrameSubmitted(ID3D12Fence* fence, uint64_t value, ID3D12CommandQueue* queue) WWMI_DEBUG_BODY();
    // Reads every region whose fence has passed; the sums land in g_gt_ms / g_gt_n for ProfFrameEnd.
    WWMI_DEBUG_FN void PresentTick(ID3D12Device* dev) WWMI_DEBUG_BODY();

private:
    bool                                    Ensure(ID3D12Device* dev);
    void                                    ReadRegion(UINT r);
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> heap_;
    Microsoft::WRL::ComPtr<ID3D12Resource>  rb_;                // the readback: 2 stamps per pair, mapped per read
    double                                  ms_per_tick_ = 0.0; // from the queue's timestamp frequency
    std::atomic<UINT>                       region_{ 0 };       // where Begin lands now
    std::atomic<UINT>                       next_pair_[kRing] = {};
    uint8_t                                 pair_bucket_[kRing][kPairs] = {};
    uint64_t                                region_fence_[kRing] = {}; // present-pass fence value stamped at the region's close
    bool                                    region_open_[kRing] = {};  // stamped and not yet read
    Microsoft::WRL::ComPtr<ID3D12Fence>     fence_;                    // the present pass's, held for GetCompletedValue
    uint64_t                                last_mark_ = 0;            // the last region's [Present] start stamp
    UINT                                    last_mark_region_ = 0;
};
WWMI_DEBUG_VAR GpuTimer g_gpu_timer;

// One bracket over a scope: the end stamp lands after the last command the scope recorded.
struct GpuBracket {
    ID3D12GraphicsCommandList* cl;
    int                        pair;
    void*                      hp;
    GpuBracket(ID3D12GraphicsCommandList* c, int bucket)
        : cl(c),
          pair(g_gpu_timer.Begin(c, bucket)),
          hp(HotPath::GpuBracketBegin(c, bucket))
    {}
    ~GpuBracket()
    {
        HotPath::GpuBracketEnd(cl, hp);
        g_gpu_timer.End(cl, pair);
    }
    GpuBracket(const GpuBracket&) = delete;
    GpuBracket& operator=(const GpuBracket&) = delete;
};

// This frame's readback, taken by ProfFrameEnd (ns summed over the pairs read, pairs counted).
inline std::atomic<uint64_t> g_gt_ns[GT_COUNT];
inline std::atomic<uint32_t> g_gt_n[GT_COUNT];
inline std::atomic<uint64_t> g_gt_frame_ns{ 0 }; // [Present] start to the next region's: GPU frame period
inline std::atomic<uint32_t> g_gt_frame_n{ 0 };
inline std::atomic<uint32_t> g_gt_frames_read{ 0 }; // regions read this frame (0, 1 or, after a stall, more)
inline std::atomic<uint32_t> g_gt_dropped{ 0 };     // regions reused before their fence passed (never read)
