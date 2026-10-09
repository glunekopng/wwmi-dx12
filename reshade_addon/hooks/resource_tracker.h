#pragma once
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>
#include <reshade.hpp>
#include <d3d12.h>
#include "descriptor_shadow.h" // ResInfo -- the value type of the maps below

// Buffer GPU address -> resource, and back. Guarded by g_mem_mutex.
extern std::mutex                             g_mem_mutex;
extern std::unordered_map<uint64_t, ResInfo>  g_va_to_res;
extern std::unordered_map<uint64_t, uint64_t> g_res_to_va;

bool BufferLive(const void* res); // a buffer with a GPU address (g_res_to_va)

void     on_barrier(reshade::api::command_list* cmd_list, uint32_t count,
    const reshade::api::resource* res, const reshade::api::resource_usage* old_s,
    const reshade::api::resource_usage* new_s);
uint32_t ResourceStateOf(void* d3d12_resource);
bool     on_create_resource(reshade::api::device* dev, reshade::api::resource_desc& desc,
    reshade::api::subresource_data*, reshade::api::resource_usage);
void     on_init_resource(reshade::api::device* device, const reshade::api::resource_desc& desc,
    const reshade::api::subresource_data* initial_data,
    reshade::api::resource_usage initial_state, reshade::api::resource res);
void     on_destroy_resource(reshade::api::device* device, reshade::api::resource res);
bool     on_copy_buffer_region(reshade::api::command_list*, reshade::api::resource source, uint64_t source_offset,
    reshade::api::resource dest, uint64_t dest_offset, uint64_t size);
void     on_init_resource_view(reshade::api::device* device, reshade::api::resource res,
    reshade::api::resource_usage            usage_type,
    const reshade::api::resource_view_desc& desc,
    reshade::api::resource_view             view);
bool     on_update_descriptor_tables(reshade::api::device* device, uint32_t count,
    const reshade::api::descriptor_table_update* updates);
bool     on_copy_descriptor_tables(reshade::api::device* device, uint32_t count,
    const reshade::api::descriptor_table_copy* copies);

// Re-run the placement swap on the slot resH was last placed in. True = the slot now holds a mod SRV.
bool RestampResource(uint64_t resH);
void ResetDescriptorTracking();
// Put the game's own descriptor back in resH's recorded slot. True = restored. log=false for bulk callers.
bool UnstampResource(uint64_t resH, bool log = true);
// The same for one heap slot, whatever resource it holds. True = restored.
bool                                      UnstampSlot(uint64_t heap_h, uint32_t offset, bool log = false);
void                                      BackupBeforeStamp(uint64_t heap_h, uint32_t offset, uint64_t mod_srv);
std::vector<std::pair<HeapKey, uint64_t>> StampedCpuOnlySlots();
// The tracked heap slot a CPU descriptor handle points into. False = no heap we track holds it.
bool TrackedSlotOfCpu(uint64_t cpu, HeapKey* out);
struct UnstampTally {
    unsigned recorded = 0;
    unsigned cpu_only = 0;
    size_t   cpu_only_stamped = 0;
};
UnstampTally UnstampAll(void (*hold)(uint64_t res));
void         SetAddonDisabled(bool dis);
