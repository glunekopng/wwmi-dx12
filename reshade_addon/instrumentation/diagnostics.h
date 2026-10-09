#pragma once
#include <reshade.hpp>
#include <d3d12.h>
#include <wrl.h>
#include <atomic>
#include <mutex>
#include <vector>
#include <set>
#include <unordered_set>
#include <unordered_map>
#include <string>
#include <cstdint>
#include "diag.h"

// --- diagnostic control flags (armed by the overlay) ---------------------------
inline std::atomic<int>      g_log_all_draws{ 0 };
inline std::atomic<int>      g_texflip_arm{ 0 };
inline std::atomic<unsigned> g_texflip_gen{ 0 };
inline std::atomic<int>      g_texnoswap_arm{ 0 }; // frames left: TEXNOSWAP logs target-shaped placements left vanilla
inline std::atomic<int>      g_texnoswap_n{ 0 };   // lines this arm (capped in TexNoSwapNote)
inline std::atomic<unsigned> g_texnoswap_gen{ 0 }; // bumped per arm: TexNoSwapNote's once-per-resource set restarts
inline std::atomic<int>      g_log_merged{ 0 };
inline std::atomic<int>      g_occl_arm{ 0 };
inline std::atomic<bool>     g_wwmi_parse{ false }; // overlay button: log each mod's parsed layout (self-check)
inline std::atomic<bool>     g_log_lod{ false };    // persistent CDRAW capture: walk through LODs, log each char draw
inline std::atomic<int>      g_shaderdiff_arm{ 0 }; // frames left in a SHADERDIFF capture

// --- UAV stalker / discovery state ---------------------------------------------
inline std::unordered_set<uint64_t> g_char_vb_vas;
inline std::mutex                   g_char_vb_mutex;
struct StalkerResult {
    uint64_t cs_hash;
    uint32_t root;
    uint32_t slot;
    bool     operator<(const StalkerResult& o) const
    {
        if (cs_hash != o.cs_hash) return cs_hash < o.cs_hash;
        if (root != o.root) return root < o.root;
        return slot < o.slot;
    }
};
inline std::set<StalkerResult> g_stalker_results;
inline std::mutex              g_stalker_mutex;

// --- shader bytecode cache (for offline DXIL disassembly) ----------------------
struct CachedShader {
    std::vector<uint8_t> code;
    char                 stage[4];
}; // stage = "vs"/"ps"/"cs"
inline std::mutex                                 g_shader_cache_mutex;
inline std::unordered_map<uint64_t, CachedShader> g_shader_cache;
inline std::atomic<bool>                          g_dump_shaders_requested{ false };

// --- functions -----------------------------------------------------------------
WWMI_DEBUG_FN void CacheShaderBytecode(uint64_t hash, const void* code, size_t code_size, const char* stage) WWMI_DEBUG_BODY();
WWMI_DEBUG_FN void DumpCachedShaders() WWMI_DEBUG_BODY();

struct MergedComp;
WWMI_DEBUG_FN void DiagOnDrawIndexed(reshade::api::command_list* cmd_list, ID3D12GraphicsCommandList* d3d12_cl,
    uint64_t vs_hash, uint32_t first_index, uint32_t index_count, MergedComp* mc,
    int32_t vertex_offset = 0, int b4_root = -1, int b3_root = -1,
    uint32_t instance_count = 1, uint32_t first_instance = 0) WWMI_DEBUG_BODY();
WWMI_DEBUG_FN void DiagOnPresent(ID3D12CommandQueue* present_queue) WWMI_DEBUG_BODY();
WWMI_DEBUG_FN void ShaderDiffNote(reshade::api::command_list* cmd_list, uint64_t vs_hash, uint32_t index_count) WWMI_DEBUG_BODY();
