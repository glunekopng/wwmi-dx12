#pragma once
// The present path's parts, called in order by on_present_body (present_hook.cpp).
#include <cstdint>
#include <reshade.hpp>
#include <string>
#include "../instrumentation/diag.h"

void               PresentQueueDiag(reshade::api::command_queue* queue);                             // present_diag.cpp
void               PortProbeTick(reshade::api::command_queue* queue, reshade::api::swapchain* sc);   // present_engine.cpp
void               ConfigWatchTick();                                                                // present_diag.cpp
void               SeedHeartbeat();                                                                  // present_diag.cpp
void               EngineFrameTick(reshade::api::command_queue* queue, reshade::api::swapchain* sc); // present_engine.cpp
void               PresenceTick();                                                                   // presence_tick.cpp
void               ModTexturesTick();                                                                // present_mod_load.cpp
void               Heartbeat(uint32_t texinj_last);                                                  // present_diag.cpp
void               HealthHeartbeat();                                                                // present_diag.cpp
void               LoadGeometryMods();                                                               // present_mod_load.cpp
WWMI_DEBUG_FN void SampleMemoryOncePerSecond() WWMI_DEBUG_BODY();                                    // profiler_mem.cpp

extern std::wstring g_cfg_engine_path;
extern std::wstring g_cfg_db_path;

void on_present_body(reshade::api::command_queue* queue, reshade::api::swapchain* sc);
